//! I5 Camera Raw settings / Affinity / SVG reference vectors (`rustref io-extra-vectors`),
//! checked by tests/io/test_extra_parity.c. Lines are tab-separated; the first field names the kind.
//!
//!   cri CASE HEX RESULT          smart_map::filter_from_item of a filterFX item (Camera Raw cases):
//!                                RESULT = the SmartFilter as serde JSON, then `rt` + the round trip
//!                                (item_for_filter: `=` when equal to the input, else `ok LEN H16`
//!                                or `err "msg"`)
//!   cre CASE JSON RESULT         item_for_filter of a Camera Raw filter with these params:
//!                                `ok LEN H16 HEX` or `err "msg"`
//!   af NAME B64|PATH RESULT      io::import of a synthetic (base64) or corpus (path) Affinity file
//!   svg NAME B64 RESULT          io::import of an SVG; raster layers listed without pixels
//!   svgpx NAME W H B64           the flattened import as RGBA8 (compared with a tolerance)
//!   svgr NAME SCALE RESULT       svg::rasterize: `W H B64(rgba8)` or `err "msg"`
//!
//! RESULT of an import: `ok DOC WARNINGS ro pv` or `err KIND "message"`. DOC lists the size, mode,
//! depth, dpi bits and every layer with its serde parts (paths, fills, strokes, text runs, masks,
//! artboards) and surfaces (format, bounds, default pixel, BLAKE3 of the bytes).
use base64::Engine;
use photocraft_color::BlendMode;
use photocraft_doc::{Document, Layer, LayerContent, SmartFilter, SmartSource};
use photocraft_io::{ImportResult, IoError};
use photocraft_psd::descriptor::{Descriptor, Id, UnicodeString, Value};
use photocraft_raster::Surface;
use serde_json::{json, Value as J};

fn h16(b: &[u8]) -> String {
    blake3::hash(b).to_hex()[..16].to_string()
}
fn b64(b: &[u8]) -> String {
    base64::engine::general_purpose::STANDARD.encode(b)
}
fn hexs(b: &[u8]) -> String {
    b.iter().map(|v| format!("{v:02x}")).collect()
}
fn fbits(v: f32) -> String {
    format!("{:08x}", v.to_bits())
}
macro_rules! sj {
    ($v:expr) => {
        serde_json::to_string($v).unwrap()
    };
}

// ---------------------------------------------------------------------------------------------
// document dumps
// ---------------------------------------------------------------------------------------------

fn surf(s: &Surface) -> String {
    let fmt = s.format();
    let b = s.content_bounds();
    let mut def = vec![0u8; fmt.channels() * fmt.sample.bytes()];
    photocraft_raster::encode_pixel(&fmt, &s.default_pixel(), &mut def);
    let px = if b.is_empty() { h16(&[]) } else { h16(&s.to_interleaved(b)) };
    format!("{:?}/{:?}/{}:{},{},{},{}:{}:{}", fmt.mode, fmt.sample, u8::from(fmt.alpha), b.x0, b.y0, b.x1, b.y1, hexs(&def), px)
}

fn opt_surf(s: Option<&Surface>, pixels: bool) -> String {
    match s {
        None => "-".into(),
        Some(s) if pixels => surf(s),
        Some(_) => "px".into(),
    }
}

/// One layer; `pixels` false leaves raster pixels out (SVG fallbacks are rendered differently).
fn layer(l: &Layer, pixels: bool, out: &mut String) {
    out.push('(');
    out.push_str(&sj!(&l.name));
    out.push_str(&format!(
        "|{}|{}|{}|{}|{}|{}",
        u8::from(l.visible),
        sj!(&l.locks),
        sj!(&l.blend),
        fbits(l.opacity),
        fbits(l.fill_opacity),
        u8::from(l.clipped)
    ));
    match &l.mask {
        Some(m) => out.push_str(&format!(
            "|m{}:{}{}:{}:{}",
            opt_surf(Some(&m.surface), pixels),
            u8::from(m.enabled),
            u8::from(m.linked),
            fbits(m.density),
            fbits(m.feather)
        )),
        None => out.push_str("|-"),
    }
    out.push('|');
    out.push_str(&sj!(&l.vector_mask));
    out.push('|');
    match &l.content {
        LayerContent::Group(g) => {
            out.push_str(&format!("G{}:{}[", u8::from(g.expanded), sj!(&g.artboard)));
            for c in &g.children {
                layer(c, pixels, out);
            }
            out.push(']');
        }
        LayerContent::Shape(s) => {
            out.push_str(&format!("S{}|{}|{}|{}", sj!(&s.path), sj!(&s.fill), sj!(&s.stroke), opt_surf(s.cache.as_ref(), true)));
        }
        LayerContent::Text(t) => {
            out.push_str(&format!(
                "T{}|{}|{}|{}|{}|{}|{}|{}|{}",
                sj!(&t.text),
                sj!(&t.font_family),
                fbits(t.size_pt),
                sj!(&t.color),
                sj!(&t.transform),
                sj!(&t.runs),
                sj!(&t.paragraphs),
                sj!(&t.shape),
                opt_surf(t.cache.as_ref(), pixels)
            ));
        }
        LayerContent::Smart(sm) => {
            let src = match &sm.source {
                SmartSource::Embedded { file_name, bytes } => format!("E{}:{}:{}", sj!(file_name), bytes.len(), h16(bytes)),
                SmartSource::Linked { path } => format!("L{}", sj!(path)),
            };
            out.push_str(&format!("O{}|{}|{}|{}", src, sj!(&sm.transform), sj!(&sm.smart_filters), opt_surf(sm.cache.as_ref(), pixels)));
        }
        LayerContent::Raster(s) => {
            out.push('R');
            out.push_str(&opt_surf(Some(s), pixels));
        }
        _ => out.push('?'),
    }
    out.push(')');
}

fn doc_sum(d: &Document, pixels: bool) -> String {
    let mut s = format!("{}:{}x{}:{}:{}:{}:", sj!(&d.name), d.size.width, d.size.height, fbits(d.resolution_dpi), sj!(&d.mode), sj!(&d.depth));
    for l in &d.layers {
        layer(l, pixels, &mut s);
    }
    s
}

fn err_kind(e: &IoError) -> &'static str {
    match e {
        IoError::Psd(_) => "Psd",
        IoError::Codec(_) => "Codec",
        IoError::UnknownFormat(_) => "UnknownFormat",
        IoError::Unsupported(_) => "Unsupported",
        IoError::Pcraft(_) => "Pcraft",
        IoError::Raw(_) => "Raw",
        IoError::Svg(_) => "Svg",
        IoError::Cancelled => "Cancelled",
        #[allow(unreachable_patterns)]
        _ => "Other",
    }
}

fn imp(r: &Result<ImportResult, IoError>, pixels: bool) -> String {
    match r {
        Ok(r) => format!("ok {} {} {} {}", doc_sum(&r.document, pixels), sj!(&r.warnings), u8::from(r.source_read_only), u8::from(r.preview_only)),
        Err(e) => format!("err {} {}", err_kind(e), sj!(&e.to_string())),
    }
}

// ---------------------------------------------------------------------------------------------
// Camera Raw
// ---------------------------------------------------------------------------------------------

const CLASS: &str = "Adobe Camera Raw Filter";
const COMMAND: &str = "filter.cameraRaw";

fn set(d: &mut Descriptor, key: &str, v: Value) {
    match d.items.iter_mut().find(|(k, _)| k.is(key)) {
        Some(e) => e.1 = v,
        None => d.items.push((Id::new(key), v)),
    }
}

fn get_desc(d: &Descriptor, key: &str) -> Descriptor {
    match d.get(key) {
        Some(Value::Descriptor(x)) => x.clone(),
        _ => panic!("no {key}"),
    }
}

fn rgbc(v: f64) -> Value {
    Value::Descriptor(Descriptor::new("RGBC").with("Rd  ", Value::Double(v)).with("Grn ", Value::Double(v)).with("Bl  ", Value::Double(v)))
}

fn blend_options(opacity: f64, mode: &str) -> Value {
    Value::Descriptor(
        Descriptor::new("blendOptions")
            .with("Opct", Value::UnitFloat { unit: *b"#Prc", value: opacity })
            .with("Md  ", Value::Enumerated { type_id: Id::new("BlnM"), value: Id::new(mode) }),
    )
}

const HSL: [[&str; 8]; 3] = [
    ["HA_R", "HA_O", "HA_Y", "HA_G", "HA_A", "HA_B", "HA_P", "HA_M"],
    ["SA_R", "SA_O", "SA_Y", "SA_G", "SA_A", "SA_B", "SA_P", "SA_M"],
    ["LA_R", "LA_O", "LA_Y", "LA_G", "LA_A", "LA_B", "LA_P", "LA_M"],
];
const NEUTRAL_ONLY: &[(&str, i32)] = &[("LNRC", 0), ("CNRS", 50), ("MDis", 0), ("VigA", 0), ("VigM", 50), ("crfs", 100), ("TMMs", 0), ("PGTM", 0), ("RGBt", 0)];

fn source_item() -> Descriptor {
    let mut d = Descriptor::new(CLASS)
        .with("CrVe", Value::Text(UnicodeString::new_nul("18.4")))
        .with("PrVN", Value::Integer(6))
        .with("PrVe", Value::Integer(251920384))
        .with("Ex12", Value::Double(1.15));
    for (key, value) in [("Cr12", -38), ("Hi12", 35), ("Sh12", -50), ("Wh12", 49), ("Bk12", -34), ("PC_S", 0), ("PC_D", -21), ("PC_L", 38), ("PC_H", 0), ("PC_1", 25), ("PC_2", 50), ("PC_3", 75)] {
        d = d.with(key, Value::Integer(value));
    }
    for keys in HSL {
        for key in keys {
            let value = match key {
                "HA_R" => -50,
                "HA_Y" => 29,
                "HA_G" => 1,
                _ => 0,
            };
            d = d.with(key, Value::Integer(value));
        }
    }
    Descriptor::new("filterFX")
        .with("Nm  ", Value::Text(UnicodeString::new_nul("Camera Raw Filter")))
        .with("blendOptions", blend_options(100.0, "normal"))
        .with("enab", Value::Boolean(true))
        .with("hasoptions", Value::Boolean(true))
        .with("FrgC", rgbc(0.0))
        .with("BckC", rgbc(255.0))
        .with("Fltr", Value::Descriptor(d))
        .with("filterID", Value::Integer(2783))
}

fn all_controls_item() -> Descriptor {
    let mut item = source_item();
    let mut d = get_desc(&item, "Fltr");
    set(&mut d, "WBal", Value::Enumerated { type_id: Id::new("WBal"), value: Id::new("Cst ") });
    set(&mut d, "ShpR", Value::Double(1.2));
    for (key, value) in [
        ("Temp", -26), ("Tint", -25), ("CrTx", -31), ("Cl12", 30), ("Dhze", -34), ("Vibr", 33), ("Strt", 33), ("Shrp", 43), ("ShpD", 36),
        ("ShpM", 30), ("LNR ", 47), ("LNRD", 50), ("CNR ", 25), ("CNRD", 50), ("STSH", 117), ("STSS", 52), ("STHH", 316), ("STHS", 70),
        ("STB ", 35), ("CgMH", 51), ("CgMS", 51), ("CgSL", -24), ("CgML", 41), ("CgHL", 28), ("CgBl", 36), ("CgGH", 20), ("CgGS", 61),
        ("CgGL", 45), ("GRNA", 45), ("GRNS", 25), ("GRNF", 50), ("PCVA", 31), ("PCVM", 50), ("PCVF", 50), ("PCVR", 0),
    ] {
        set(&mut d, key, Value::Integer(value));
    }
    for (key, neutral) in NEUTRAL_ONLY {
        set(&mut d, key, Value::Integer(*neutral));
    }
    for (key, x, y) in [("Crv ", 146, 102), ("CrvR", 90, 174), ("CrvG", 162, 122), ("CrvB", 129, 202)] {
        set(&mut d, key, Value::List([0, 0, x, y, 255, 255].into_iter().map(Value::Integer).collect()));
    }
    set(&mut item, "Fltr", Value::Descriptor(d));
    item
}

fn with_fltr(mut item: Descriptor, key: &str, v: Value) -> Descriptor {
    let mut d = get_desc(&item, "Fltr");
    set(&mut d, key, v);
    set(&mut item, "Fltr", Value::Descriptor(d));
    item
}

fn export_line(r: Result<Descriptor, String>) -> String {
    match r {
        Ok(d) => {
            let b = d.to_bytes();
            format!("ok {} {} {}", b.len(), h16(&b), hexs(&b))
        }
        Err(e) => format!("err {}", sj!(&e)),
    }
}

fn camera_raw() {
    let mut items: Vec<(String, Descriptor)> = vec![("source".into(), source_item()), ("all".into(), all_controls_item())];
    items.push(("source+outer".into(), source_item().with("futureOuterData", Value::RawData(vec![1, 2, 3, 4]))));
    for (key, neutral) in NEUTRAL_ONLY {
        items.push((format!("neutral-{}", key.trim()), with_fltr(all_controls_item(), key, Value::Integer(neutral + 1))));
        items.push((format!("neutral-ok-{}", key.trim()), with_fltr(source_item(), key, Value::Integer(*neutral))));
    }
    let curves: Vec<Value> = vec![
        Value::List(vec![Value::Integer(0)]),
        Value::List([0, 0, 0, 255].into_iter().map(Value::Integer).collect()),
        Value::List([0, 0, 255, 256].into_iter().map(Value::Integer).collect()),
        Value::List(vec![Value::Double(0.0); 4]),
        Value::List(vec![Value::Integer(0); 34]),
        Value::List([0, 0, 255, 255].into_iter().map(Value::Integer).collect()),
        Value::List([0, 0, 10, 40, 255, 255].into_iter().map(Value::Integer).collect()),
        Value::Integer(3),
    ];
    for (i, v) in curves.into_iter().enumerate() {
        items.push((format!("curve-{i}"), with_fltr(all_controls_item(), "Crv ", v)));
    }
    for (i, (key, value)) in [
        ("LCs ", Value::Text(UnicodeString::new("unknown masks"))),
        ("PrVN", Value::Integer(7)),
        ("PrVe", Value::Integer(123)),
        ("Ex12", Value::Double(f64::INFINITY)),
        ("Ex12", Value::Integer(1)),
        ("Ex12", Value::Double(5.5)),
        ("Cr12", Value::Text(UnicodeString::new("bad type"))),
        ("Cr12", Value::Integer(101)),
        ("Cr12", Value::Double(10.0)),
        ("ShpR", Value::Double(0.2)),
        ("STSH", Value::Integer(360)),
        ("WBal", Value::Enumerated { type_id: Id::new("WBal"), value: Id::new("AsSh") }),
        ("CrVe", Value::Integer(18)),
        ("PC_1", Value::Integer(60)),
    ]
    .into_iter()
    .enumerate()
    {
        items.push((format!("bad-{i}-{}", key.trim()), with_fltr(source_item(), key, value)));
    }
    {
        let mut v5 = source_item();
        let mut d = get_desc(&v5, "Fltr");
        set(&mut d, "PrVN", Value::Integer(5));
        set(&mut d, "PrVe", Value::Integer(184549376));
        set(&mut v5, "Fltr", Value::Descriptor(d));
        items.push(("process5".into(), v5));
        let mut id = source_item();
        set(&mut id, "filterID", Value::Integer(1));
        items.push(("wrong-id".into(), id));
        let mut blend = source_item();
        set(&mut blend, "blendOptions", blend_options(40.0, "mltp"));
        set(&mut blend, "enab", Value::Boolean(false));
        items.push(("blended".into(), blend));
    }
    for (name, item) in &items {
        let f = photocraft_io::smart_map::filter_from_item(item);
        let rt = match photocraft_io::smart_map::item_for_filter(&f) {
            Ok(d) if &d == item => "=".to_string(),
            Ok(d) => {
                let b = d.to_bytes();
                format!("ok {} {}", b.len(), h16(&b))
            }
            Err(e) => format!("err {}", sj!(&e)),
        };
        println!("cri\t{name}\t{}\t{}\trt {rt}", hexs(&item.to_bytes()), sj!(&f));
    }
    // Edits of an imported filter and new filters.
    let source = photocraft_io::smart_map::filter_from_item(&all_controls_item());
    let mut params: Vec<(String, J)> = Vec::new();
    for (k, v) in [
        ("exposure", json!(0.0)),
        ("exposure", json!(1.5)),
        ("temperature", json!(12.4)),
        ("contrast", json!(-12.5)),
        ("gradeShadows", json!({"hue": 359.7, "sat": 10, "lum": 3})),
        ("hslHue", json!([1, 2, 3, 4, 5, 6, 7, 8])),
        ("pointCurve", json!([])),
        ("pointCurve", json!([[0, 0], [100.2, 120.6], [200.6, 180.4], [255, 255]])),
        ("pointCurveRed", json!([[0, 0], [255, 255]])),
        ("curveSplits", json!([20, 45, 80])),
        ("sharpenRadius", json!(2.25)),
        ("seed", json!(5)),
        ("vignetteStyle", json!("colorPriority")),
        ("layer", json!(3)),
        ("__private", json!("x")),
    ] {
        let mut p = source.params.clone();
        p[k] = v;
        params.push((format!("edit-{k}"), p));
    }
    for (i, p) in [
        J::Null,
        json!({}),
        json!({"exposure": "bright"}),
        json!({"exposure": 1e40}),
        json!({"exposure": 6}),
        json!({"contrast": 101}),
        json!({"hslHue": [1, 2]}),
        json!({"curveSplits": [80, 50, 20]}),
        json!({"curveSplits": [25, 25, 75]}),
        json!({"pointCurve": [[0, 0], [0, 255]]}),
        json!({"pointCurve": [[0, 0], [255, 255], [256, 255]]}),
        json!({"pointCurve": (0..17).map(|i| [i * 15, i * 15]).collect::<Vec<_>>()}),
        json!({"sharpenRadius": 4}),
        json!({"gradeShadows": {"hue": 361}}),
        json!({"gradeShadows": {"unknown": 1}}),
        json!({"seed": 1}),
        json!({"vignetteHighlights": 10}),
        json!({"futureSetting": 1}),
        json!({"__cameraRawPsd": "zz"}),
        json!({"__cameraRawPsd": "00"}),
        json!({"__cameraRawPsd": 12}),
        json!({"__cameraRawPsd": hexs(&Descriptor::new("filterFX").with("filterID", Value::Integer(1)).to_bytes())}),
        json!({"exposure": 0.75, "sharpenRadius": 1.2, "contrast": 12, "temperature": -3, "hslSat": [1, 2, 3, 4, 5, 6, 7, 8], "seed": 0}),
        json!({"pointCurveBlue": [[0, 0], [128, 140], [255, 255]], "gradeGlobal": {"hue": 20, "sat": 61, "lum": 45}}),
    ]
    .into_iter()
    .enumerate()
    {
        params.push((format!("new-{i}"), p));
    }
    for (name, p) in params {
        let f = SmartFilter { command: COMMAND.into(), params: p.clone(), blend: BlendMode::Normal, opacity: 1.0, visible: true };
        println!("cre\t{name}\t{}\t{}", sj!(&p), export_line(photocraft_io::smart_map::item_for_filter(&f)));
    }
}

// ---------------------------------------------------------------------------------------------
// Affinity
// ---------------------------------------------------------------------------------------------

/// A field value of the synthetic stream (copied from crates/affinity/src/synth.rs, which is
/// behind a cargo feature rustref doesn't enable).
#[derive(Clone)]
enum F {
    U8(u8),
    U32(u32),
    F64(f64),
    Bool(bool),
    Enum(u16, u16),
    Str(String),
    F64s(Vec<f64>),
    Struct(Vec<u8>),
    Records(u16, Vec<Vec<u8>>),
    Def(u32, Vec<[u8; 4]>, Vec<([u8; 4], F)>),
    Obj([u8; 4], Vec<([u8; 4], F)>),
    Pos(Vec<F>),
    Shared(Vec<F>),
}

fn put_tag(out: &mut Vec<u8>, t: [u8; 4]) {
    out.extend(u32::from_be_bytes(t).to_le_bytes());
}

fn code(f: &F) -> u8 {
    match f {
        F::U8(_) => 0x01,
        F::U32(_) => 0x03,
        F::F64(_) => 0x0a,
        F::Bool(_) => 0x29,
        F::Enum(..) => 0x2a,
        F::Str(_) => 0x2b,
        F::F64s(v) => 0x22 + v.len() as u8,
        F::Struct(b) => 0x34 + b.len() as u8,
        F::Records(..) => 0x80 | 0x2c,
        F::Def(..) => 0x31,
        F::Obj(..) => 0x32,
        F::Pos(_) => 0x30,
        F::Shared(_) => 0x80 | 0x31,
    }
}

fn payload(out: &mut Vec<u8>, f: &F) {
    match f {
        F::U8(v) => out.push(*v),
        F::U32(v) => out.extend(v.to_le_bytes()),
        F::F64(v) => out.extend(v.to_le_bytes()),
        F::Bool(v) => out.push(u8::from(*v)),
        F::Enum(id, ver) => {
            out.extend(id.to_le_bytes());
            out.extend(ver.to_le_bytes());
        }
        F::Str(s) => {
            out.extend((s.len() as u32).to_le_bytes());
            out.extend(s.as_bytes());
        }
        F::F64s(v) => v.iter().for_each(|x| out.extend(x.to_le_bytes())),
        F::Struct(b) => out.extend(b),
        F::Records(size, recs) => {
            out.extend((recs.len() as u32).to_le_bytes());
            out.extend(size.to_le_bytes());
            recs.iter().for_each(|r| out.extend(r));
        }
        F::Def(id, chain, fields) => {
            out.push(1);
            out.extend(id.to_le_bytes());
            let (last, rest) = chain.split_last().unwrap();
            for t in rest {
                out.push(0);
                put_tag(out, *t);
                out.extend(0u16.to_le_bytes());
                out.push(0);
            }
            out.push(1);
            put_tag(out, *last);
            fields_into(out, fields);
        }
        F::Obj(class, fields) => {
            out.push(1);
            put_tag(out, *class);
            out.extend(0u16.to_le_bytes());
            fields_into(out, fields);
        }
        F::Pos(items) => {
            for f in items {
                out.push(code(f));
                payload(out, f);
            }
            out.push(0);
        }
        F::Shared(items) => {
            out.extend((items.len() as u32).to_le_bytes());
            items.iter().for_each(|f| payload(out, f));
        }
    }
}

fn fields_into(out: &mut Vec<u8>, fields: &[([u8; 4], F)]) {
    for (t, f) in fields {
        out.push(code(f));
        put_tag(out, *t);
        payload(out, f);
    }
    out.push(0);
}

fn stream(root: &[([u8; 4], F)]) -> Vec<u8> {
    let mut out = b"\x00\xffKS".to_vec();
    out.extend(2u16.to_le_bytes());
    put_tag(&mut out, *b"Pers");
    out.extend(0u16.to_le_bytes());
    out.extend(30u32.to_le_bytes());
    fields_into(&mut out, root);
    out
}

fn crc32(data: &[u8]) -> u32 {
    let mut c = 0xffff_ffffu32;
    for &b in data {
        c ^= u32::from(b);
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0xedb8_8320 } else { c >> 1 };
        }
    }
    !c
}

/// A version-12 container with a stored doc.dat and the thumbnail (synth::container, Stored).
fn container(doc: &[u8], png: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; 72];
    out[..4].copy_from_slice(photocraft_affinity::MAGIC);
    out[4..6].copy_from_slice(&12u16.to_le_bytes());
    out[6..8].copy_from_slice(&4u16.to_le_bytes());
    out[8..12].copy_from_slice(b"nsrP");
    out[12..16].copy_from_slice(b"#Inf");
    out[64..68].copy_from_slice(b"Prot");
    let offset = out.len() as u64;
    out.extend(b"#Fil");
    out.extend(doc);
    let mut r = Vec::new();
    r.extend(1u32.to_le_bytes());
    r.push(0);
    r.extend(offset.to_le_bytes());
    r.extend((doc.len() as u64).to_le_bytes());
    r.extend((doc.len() as u64).to_le_bytes());
    r.extend(crc32(doc).to_le_bytes());
    r.push(0);
    r.extend(0u32.to_le_bytes());
    r.extend(0u32.to_le_bytes());
    r.extend(7u16.to_le_bytes());
    r.extend(b"doc.dat");
    let fat = out.len() as u64;
    out.extend(b"#FT4");
    out.extend(0u64.to_le_bytes());
    out.extend(1_700_000_000u64.to_le_bytes());
    out.extend([0u8; 24]);
    out.extend(1u32.to_le_bytes());
    out.extend([0u8; 8]);
    out.extend(0u16.to_le_bytes());
    out.push(0);
    out.extend(&r);
    let thumb = out.len() as u64;
    out[16..24].copy_from_slice(&fat.to_le_bytes());
    out[24..32].copy_from_slice(&thumb.to_le_bytes());
    out.extend(b"\xff\xff\xff\xffThmb");
    out.extend(1u32.to_le_bytes());
    out.extend((png.len() as u32 + 13).to_le_bytes());
    out.extend(29u32.to_le_bytes());
    out.extend(0u32.to_le_bytes());
    out.extend((png.len() as u32).to_le_bytes());
    out.push(1);
    out.extend(png);
    out
}

fn png() -> Vec<u8> {
    use photocraft_codecs::{ChannelLayout, EncodeOptions, Format, Image, SampleType};
    let img = Image::from_raw(2, 1, ChannelLayout::Rgba, SampleType::U8, vec![220, 40, 60, 255, 0, 0, 0, 0]).unwrap();
    photocraft_codecs::encode(&img, Format::Png, &EncodeOptions::default()).unwrap()
}

fn rgba(r: f32, g: f32, b: f32, a: f32) -> F {
    F::Struct([r, g, b, a].iter().flat_map(|v| v.to_le_bytes()).collect())
}

fn solid(id: u32, c: F) -> F {
    F::Def(id, vec![*b"FDsc"], vec![(*b"FDeF", F::Def(id + 1, vec![*b"FilS"], vec![(*b"Colr", F::Def(id + 2, vec![*b"RGBA"], vec![(*b"_col", c)]))]))])
}

fn knot(x: f64, y: f64) -> Vec<u8> {
    let mut r = x.to_le_bytes().to_vec();
    r.extend(y.to_le_bytes());
    r.extend([1, 0]);
    r
}

fn square(s: f64, o: f64) -> Vec<Vec<u8>> {
    vec![knot(o, o), knot(o + s, o), knot(o + s, o + s), knot(o, o + s), knot(o, o)]
}

fn curve(id: u32, name: &str, extra: Vec<([u8; 4], F)>, subs: Vec<(f64, f64)>, children: Vec<F>) -> F {
    let mut pos = vec![F::U8(0), F::U32(subs.len() as u32)];
    for (s, o) in &subs {
        pos.push(F::Bool(true));
        pos.push(F::Records(18, square(*s, *o)));
    }
    let mut fields = vec![(*b"Desc", F::Str(name.into())), (*b"Crvs", F::Obj(*b"PCvD", vec![(*b"Data", F::Pos(pos))]))];
    fields.extend(extra);
    if !children.is_empty() {
        fields.push((*b"Chld", F::Shared(children)));
    }
    F::Def(id, vec![*b"PCrv"], fields)
}

fn stroke(id: u32, w: f64) -> F {
    F::Shared(vec![F::Def(id, vec![*b"LDsc"], vec![(*b"LDeL", F::Def(id + 1, vec![*b"LSty"], vec![(*b"Wght", F::F64(w))]))])])
}

fn affinity_doc(spreads: Vec<F>, dpi: f64) -> Vec<u8> {
    let doc = stream(&[
        (*b"UVCn", F::Obj(*b"UVCn", vec![(*b"UPPI", F::F64(dpi))])),
        (*b"DocR", F::Def(1, vec![*b"DocN"], vec![(*b"Chld", F::Shared(spreads))])),
    ]);
    container(&doc, &png())
}

fn spread(id: u32, b: [f64; 4], transparent: bool, children: Vec<F>) -> F {
    F::Def(id, vec![*b"Sprd"], vec![(*b"SprB", F::F64s(b.to_vec())), (*b"SprT", F::Bool(transparent)), (*b"Chld", F::Shared(children))])
}

fn synthetic_affinity() -> Vec<(String, Vec<u8>)> {
    let ring = curve(
        20,
        "Ring",
        vec![
            (*b"BFFl", F::Shared(vec![solid(21, rgba(1.0, 0.0, 0.0, 1.0))])),
            (*b"LIFl", F::Shared(vec![solid(24, rgba(0.0, 0.0, 1.0, 1.0))])),
            (*b"LILn", stroke(27, 4.0)),
        ],
        vec![(60.0, 0.0), (20.0, 20.0)],
        vec![],
    );
    let board = |child: F, id: u32, nested: bool| {
        let f = F::Def(
            id,
            vec![*b"ShpN"],
            vec![
                (*b"Desc", F::Str("Board".into())),
                (*b"ABEn", F::Bool(true)),
                (*b"Shpe", F::Def(id + 1, vec![*b"ShNR"], vec![])),
                (*b"ShpB", F::F64s(vec![0.0, 0.0, 120.0, 80.0])),
                (*b"Xfrm", F::F64s(vec![1.0, 0.0, 20.0, 0.0, 1.0, 10.0])),
                (*b"BFFl", F::Shared(vec![solid(id + 2, rgba(1.0, 1.0, 1.0, 1.0))])),
                (*b"Chld", F::Shared(vec![child])),
            ],
        );
        if nested { F::Def(id + 10, vec![*b"Grup"], vec![(*b"Desc", F::Str("Holder".into())), (*b"Chld", F::Shared(vec![f]))]) } else { f }
    };
    let mut out = Vec::new();
    out.push(("ring.af".to_string(), affinity_doc(vec![spread(2, [0.0, 0.0, 200.0, 100.0], true, vec![board(ring.clone(), 10, false)])], 144.0)));
    out.push(("nested-board.af".to_string(), affinity_doc(vec![spread(2, [0.0, 0.0, 200.0, 100.0], true, vec![board(ring.clone(), 10, true)])], 72.0)));
    // An opaque page with a hidden, a locked, a translucent and an unknown-blend layer.
    let plain = |id: u32, name: &str, extra: Vec<([u8; 4], F)>| {
        let mut e = vec![(*b"BFFl", F::Shared(vec![solid(id + 1, rgba(0.2, 0.6, 0.3, 0.8))]))];
        e.extend(extra);
        curve(id, name, e, vec![(30.0, 5.0)], vec![])
    };
    out.push((
        "flags.afdesign".to_string(),
        affinity_doc(
            vec![spread(
                2,
                [0.0, 0.0, 64.0, 48.0],
                false,
                vec![
                    plain(30, "Hidden", vec![(*b"Visi", F::Bool(false))]),
                    plain(40, "Locked", vec![(*b"Edtb", F::Bool(false)), (*b"Opac", F::F64(0.25))]),
                    plain(50, "Blend", vec![(*b"Blnd", F::Enum(3, 1))]),
                    plain(60, "Odd blend", vec![(*b"Blnd", F::Enum(25, 1))]),
                    plain(70, "", vec![(*b"Opac", F::F64(2.0))]),
                ],
            )],
            300.0,
        ),
    ));
    // A shape clipping a child, an unknown class with and without children, a group.
    let child = plain(80, "Inside", vec![]);
    let clip = curve(90, "Clip", vec![(*b"BFFl", F::Shared(vec![solid(91, rgba(0.0, 0.0, 0.0, 1.0))])), (*b"LILn", stroke(94, 2.0))], vec![(40.0, 2.0)], vec![child]);
    let unknown = F::Def(100, vec![*b"Zzzz"], vec![(*b"Desc", F::Str("Mystery".into()))]);
    let unknown_kids = F::Def(110, vec![*b"Zzzz"], vec![(*b"Chld", F::Shared(vec![plain(120, "Kid", vec![])]))]);
    let group = F::Def(130, vec![*b"Grup"], vec![(*b"Desc", F::Str("Group".into())), (*b"Chld", F::Shared(vec![plain(140, "A", vec![]), plain(150, "B", vec![])]))]);
    out.push(("structure.afphoto".to_string(), affinity_doc(vec![spread(2, [0.0, 0.0, 50.0, 50.0], true, vec![clip, unknown, unknown_kids, group])], 72.0)));
    // Two spreads: pages become artboards side by side.
    out.push((
        "pages.afpub".to_string(),
        affinity_doc(vec![spread(2, [0.0, 0.0, 40.0, 30.0], false, vec![plain(160, "P1", vec![])]), spread(3, [10.0, 0.0, 60.0, 50.0], true, vec![plain(170, "P2", vec![])])], 96.0),
    ));
    out
}

fn affinity() {
    let synth = synthetic_affinity();
    for (name, bytes) in &synth {
        println!("af\t{name}\t{}\t{}", b64(bytes), imp(&photocraft_io::import(name, bytes), true));
    }
    // Seeded mutations of the ring document (xorshift; the C test applies the same edits).
    let base = &synth[0].1;
    let mut seed = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = || {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        seed
    };
    for m in 0..40 {
        let mut b = base.clone();
        let n = 1 + (next() % 8) as usize;
        let mut edits = Vec::new();
        for _ in 0..n {
            let i = (next() % b.len() as u64) as usize;
            let v = (next() & 0xff) as u8;
            b[i] = v;
            edits.push(format!("{i}:{v}"));
        }
        println!("afm\t{m}\t{}\t{}", edits.join(","), imp(&photocraft_io::import("mutated.af", &b), true));
    }
    // Every 37th truncation.
    for end in (0..base.len()).step_by(37) {
        println!("aft\t{end}\t{}", imp(&photocraft_io::import("cut.af", &base[..end]), true));
    }
    // The public corpus, when fetched.
    let root = concat!(env!("CARGO_MANIFEST_DIR"), "/../../corpus/affinity");
    let mut files = Vec::new();
    collect(std::path::Path::new(root), &mut files);
    files.sort();
    for p in files {
        let rel = p.strip_prefix(concat!(env!("CARGO_MANIFEST_DIR"), "/../../")).unwrap().to_string_lossy().into_owned();
        let bytes = std::fs::read(&p).unwrap();
        let name = p.file_name().unwrap().to_string_lossy().into_owned();
        // Corpus documents hold thousands of shapes: their dump is hashed (rerun rustref to diff).
        let r = photocraft_io::import(&name, &bytes);
        let dump = imp(&r, true);
        let short = match &r {
            Ok(r) => format!("ok {} {}", r.document.layers.len(), sj!(&r.warnings)),
            Err(e) => format!("err {}", sj!(&e.to_string())),
        };
        println!("afc\t{rel}\t{}\t{short}", h16(dump.as_bytes()));
    }
}

fn collect(dir: &std::path::Path, out: &mut Vec<std::path::PathBuf>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            collect(&p, out);
        } else if p.extension().and_then(|e| e.to_str()).is_some_and(|e| photocraft_io::affinity::has_extension(&format!("x.{e}")) || e == "aftemplate") {
            out.push(p);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// SVG
// ---------------------------------------------------------------------------------------------

const NS: &str = "xmlns=\"http://www.w3.org/2000/svg\"";

fn png_url() -> String {
    use photocraft_codecs::{ChannelLayout, EncodeOptions, Format, Image};
    let img = Image::from_u8(2, 2, ChannelLayout::Rgb, vec![255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255]).unwrap();
    format!("data:image/png;base64,{}", b64(&photocraft_codecs::encode(&img, Format::Png, &EncodeOptions::default()).unwrap()))
}

fn svgs() -> Vec<(String, String)> {
    let w = |body: &str| format!("<svg {NS} width=\"64\" height=\"48\">{body}</svg>");
    let mut v: Vec<(&str, String)> = vec![
        ("rects", w("<rect x=\"2\" y=\"3\" width=\"20\" height=\"10\" fill=\"red\"/><rect id=\"r\" x=\"30\" y=\"5\" width=\"25\" height=\"30\" rx=\"5\" fill=\"#0f0\"/><rect x=\"5\" y=\"20\" width=\"20\" height=\"20\" rx=\"30\" ry=\"4\"/><rect width=\"-3\" height=\"4\"/><rect width=\"10\" height=\"10\" rx=\"-1\" ry=\"2\" x=\"40\" y=\"38\"/>")),
        ("circles", w("<circle cx=\"20\" cy=\"20\" r=\"12\" fill=\"blue\"/><ellipse cx=\"45\" cy=\"30\" rx=\"15\" ry=\"8\" fill=\"#123\"/><ellipse cx=\"10\" cy=\"40\" rx=\"5\"/><circle r=\"0\"/>")),
        ("lines", w("<line x1=\"1\" y1=\"2\" x2=\"60\" y2=\"40\" stroke=\"black\" stroke-width=\"3\"/><polyline points=\"1,1 10,30 20,5 30 40\" fill=\"none\" stroke=\"red\"/><polygon points=\"40 2 60 10 50 30\" fill=\"orange\"/><polyline points=\"5\"/><polygon points=\"1 2 3\"/>")),
        ("path-abs", w("<path d=\"M10 10 L50 10 H55 V40 C40 45 30 45 20 40 S5 30 10 10 Z\" fill=\"purple\"/>")),
        ("path-rel", w("<path d=\"m10 10 l20 0 h5 v20 c-5 5 -10 5 -15 0 s-5 -10 0 -20 q5 -5 10 0 t10 0 z m5 5 l3 3\" fill=\"teal\" stroke=\"black\"/>")),
        ("path-arcs", w("<path d=\"M10 30 A10 10 0 0 1 30 30 A10 5 30 1 0 50 30 a8 8 0 1 1 -10 10 A0 5 0 0 1 60 40 A5 5 0 0 1 60 40 Z\" fill=\"gold\" stroke=\"#000\"/>")),
        ("path-implicit", w("<path d=\"M1,1 20,1 20,20 1,20z M30 30 40 30 40 40\" fill=\"#aaa\" fill-rule=\"evenodd\"/><path d=\"M5 5 L 10\" stroke=\"red\"/><path d=\"L5 5\"/><path d=\"M5 5 L10 10 Z L 20 5\" stroke=\"blue\"/>")),
        ("path-quads", w("<path d=\"M2 40 Q16 2 30 40 T58 40 T60 10\" fill=\"none\" stroke=\"green\" stroke-width=\"2\"/>")),
        ("numbers", w("<path d=\"M1.5.5L1e1 2E1l-.5-.5 3e-1+2 .1.2z\" fill=\"black\" transform=\"scale(2)\"/>")),
        ("transforms", w("<g transform=\"translate(10 5) rotate(30) scale(1.5 .5)\"><rect width=\"10\" height=\"10\"/></g><rect x=\"30\" y=\"10\" width=\"8\" height=\"8\" transform=\"rotate(45 34 14)\"/><rect x=\"40\" width=\"8\" height=\"8\" transform=\"skewX(20) skewY(-10)\"/><rect y=\"30\" width=\"8\" height=\"8\" transform=\"matrix(1 .2 .3 1 2 3)\"/><rect width=\"5\" height=\"5\" transform=\"scale(0)\"/><rect x=\"50\" y=\"40\" width=\"5\" height=\"5\" transform=\"bogus(1)\"/>")),
        ("nested-groups", w("<g id=\"outer\" transform=\"translate(2,2)\"><g opacity=\"0.5\"><rect width=\"10\" height=\"10\"/><g id=\"solo\"><circle cx=\"20\" cy=\"20\" r=\"5\"/></g></g><g style=\"mix-blend-mode:screen\"><rect x=\"30\" width=\"10\" height=\"10\" fill=\"red\"/></g><g mix-blend-mode=\"multiply\"><rect y=\"30\" width=\"5\" height=\"5\"/></g></g>")),
        ("viewbox-meet", format!("<svg {NS} width=\"100\" height=\"50\" viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\" fill=\"red\"/></svg>")),
        ("viewbox-slice", format!("<svg {NS} width=\"100\" height=\"50\" viewBox=\"5 5 10 10\" preserveAspectRatio=\"xMinYMax slice\"><circle cx=\"10\" cy=\"10\" r=\"5\"/></svg>")),
        ("viewbox-none", format!("<svg {NS} width=\"100\" height=\"50\" viewBox=\"0 0 10 10\" preserveAspectRatio=\"none\"><circle cx=\"5\" cy=\"5\" r=\"5\" stroke=\"blue\" stroke-width=\"1\"/></svg>")),
        ("units", format!("<svg {NS} width=\"2in\" height=\"30mm\"><rect x=\"1cm\" y=\"10pt\" width=\"2pc\" height=\"50%\" fill=\"red\"/><g font-size=\"20\"><rect x=\"2em\" width=\"1ex\" height=\"10%\"/></g><circle cx=\"50%\" cy=\"50%\" r=\"10%\"/></svg>")),
        ("percent-size", format!("<svg {NS} width=\"50%\" height=\"80\"><rect x=\"10\" y=\"10\" width=\"30\" height=\"30\"/></svg>")),
        ("no-size", format!("<svg {NS}><rect x=\"10\" y=\"10\" width=\"130\" height=\"30\"/></svg>")),
        ("colors", w("<rect width=\"8\" height=\"8\" fill=\"#abc\"/><rect x=\"8\" width=\"8\" height=\"8\" fill=\"#11223380\"/><rect x=\"16\" width=\"8\" height=\"8\" fill=\"rgb(10%, 50%, 100%)\"/><rect x=\"24\" width=\"8\" height=\"8\" fill=\"rgba(255,0,0,0.25)\"/><rect x=\"32\" width=\"8\" height=\"8\" fill=\"hsl(120, 100%, 25%)\"/><rect x=\"40\" width=\"8\" height=\"8\" fill=\"hsla(-30, 50%, 50%, .5)\"/><rect x=\"48\" width=\"8\" height=\"8\" fill=\"DarkSlateGray\"/><rect y=\"10\" width=\"8\" height=\"8\" fill=\"transparent\" stroke=\"#f0f8\"/><rect y=\"20\" x=\"20\" width=\"8\" height=\"8\" fill=\"bogus\"/><rect y=\"20\" x=\"30\" width=\"8\" height=\"8\" stroke=\"bogus\" fill=\"none\"/><g color=\"orange\"><rect y=\"30\" width=\"8\" height=\"8\" fill=\"currentColor\"/></g>")),
        ("inherit", w("<g fill=\"red\" stroke=\"blue\" stroke-width=\"3\" opacity=\"0.5\"><rect width=\"20\" height=\"20\" fill=\"inherit\" opacity=\"inherit\"/><rect x=\"30\" width=\"20\" height=\"20\" stroke=\"inherit\" stroke-width=\"inherit\"/></g><rect y=\"30\" width=\"8\" height=\"8\" fill=\"inherit\" stroke-linecap=\"inherit\"/>")),
        ("opacity", w("<rect width=\"30\" height=\"30\" fill=\"red\" fill-opacity=\"0.5\" stroke=\"black\" stroke-opacity=\"25%\" stroke-width=\"4\" opacity=\"0.8\"/><rect x=\"35\" width=\"20\" height=\"20\" opacity=\"2\" fill-opacity=\"-1\"/>")),
        ("strokes", w("<path d=\"M5 5 L30 20 L5 35\" fill=\"none\" stroke=\"black\" stroke-width=\"4\" stroke-linejoin=\"miter\" stroke-miterlimit=\"10\" stroke-linecap=\"square\"/><path d=\"M35 5 L60 20 L35 35\" fill=\"none\" stroke=\"red\" stroke-width=\"3\" stroke-linejoin=\"round\" stroke-dasharray=\"4 2 1\" stroke-dashoffset=\"3\"/><line x1=\"5\" y1=\"42\" x2=\"60\" y2=\"42\" stroke=\"blue\" stroke-width=\"2\" stroke-dasharray=\"0 0\"/><line x1=\"5\" y1=\"46\" x2=\"60\" y2=\"46\" stroke=\"blue\" stroke-dasharray=\"5,-1\" stroke-linejoin=\"miter-clip\" stroke-miterlimit=\"0.5\"/><rect x=\"40\" y=\"30\" width=\"5\" height=\"5\" stroke=\"black\" stroke-width=\"0\"/>")),
        ("stroke-scale", w("<g transform=\"scale(2 1)\"><circle cx=\"10\" cy=\"20\" r=\"8\" fill=\"none\" stroke=\"black\" stroke-width=\"2\" stroke-dasharray=\"3 1\"/></g><g transform=\"scale(1.5)\"><rect x=\"25\" y=\"5\" width=\"10\" height=\"10\" fill=\"none\" stroke=\"red\" stroke-width=\"2\"/></g>")),
        ("display", w("<rect width=\"10\" height=\"10\" display=\"none\"/><g style=\"display:none\"><rect x=\"20\" width=\"10\" height=\"10\"/></g><rect x=\"30\" width=\"10\" height=\"10\" visibility=\"hidden\"/><g visibility=\"hidden\"><rect x=\"40\" width=\"10\" height=\"10\" visibility=\"visible\"/></g><rect y=\"20\" width=\"10\" height=\"10\" fill=\"none\"/>")),
        ("css", format!("<svg {NS} width=\"64\" height=\"48\"><style type=\"text/css\">rect {{ fill: blue; }} .a {{ fill: red }} #b {{ fill: green !important; stroke: black }} g > .c {{ stroke-width: 5 }} rect + rect {{ opacity: .5 }} rect:first-child {{ stroke: orange }} [data-x] {{ fill: pink }} @import url(x.css);</style><rect width=\"10\" height=\"10\"/><rect class=\"a\" x=\"12\" width=\"10\" height=\"10\"/><rect id=\"b\" class=\"a\" x=\"24\" width=\"10\" height=\"10\" style=\"fill: yellow\"/><g><rect class=\"c\" x=\"36\" width=\"10\" height=\"10\" style=\"fill:purple !important; stroke: red\"/></g><rect data-x=\"1\" y=\"20\" width=\"10\" height=\"10\"/></svg>")),
        ("style-attr", w("<rect width=\"20\" height=\"20\" style=\"fill:#f00;stroke:#00f;stroke-width:2px;opacity:.5;transform:translate(5px,5px)\"/><rect x=\"30\" width=\"10\" height=\"10\" fill=\"red\" style=\"fill: ; stroke\"/>")),
        ("use-symbol", w("<defs><rect id=\"r\" width=\"10\" height=\"10\" fill=\"red\"/><symbol id=\"s\" viewBox=\"0 0 10 10\"><circle cx=\"5\" cy=\"5\" r=\"5\"/></symbol><g id=\"gg\"><rect width=\"3\" height=\"3\"/><use href=\"#r\" x=\"5\"/></g></defs><use href=\"#r\" x=\"5\" y=\"5\"/><use xlink:href=\"#r\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" transform=\"translate(20 0)\" fill=\"blue\"/><use href=\"#s\" x=\"30\" y=\"20\" width=\"20\" height=\"20\"/><use href=\"#gg\" x=\"40\" y=\"2\"/><use href=\"#missing\"/><use href=\"#s\" x=\"5\" y=\"30\" width=\"10\" height=\"10\" style=\"overflow:visible\"/>")),
        ("nested-svg", w("<svg x=\"10\" y=\"10\" width=\"20\" height=\"20\" viewBox=\"0 0 10 10\"><rect width=\"20\" height=\"20\" fill=\"red\"/></svg><svg x=\"35\" y=\"5\"><circle cx=\"5\" cy=\"5\" r=\"5\"/></svg>")),
        ("switch", w("<switch><rect systemLanguage=\"fr\" width=\"10\" height=\"10\" fill=\"red\"/><rect systemLanguage=\"en-GB, de\" x=\"20\" width=\"10\" height=\"10\" fill=\"green\"/><rect x=\"40\" width=\"10\" height=\"10\"/></switch><rect requiredExtensions=\"x\" y=\"20\" width=\"10\" height=\"10\"/><rect requiredFeatures=\"http://www.w3.org/TR/SVG11/feature#Shape\" y=\"30\" width=\"10\" height=\"10\"/>")),
        ("gradient-linear", w("<defs><linearGradient id=\"g\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\"><stop offset=\"0\" stop-color=\"red\"/><stop offset=\"50%\" stop-color=\"lime\" stop-opacity=\"0.5\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient><linearGradient id=\"u\" gradientUnits=\"userSpaceOnUse\" x1=\"0\" y1=\"40\" x2=\"60\" y2=\"0\" gradientTransform=\"rotate(10)\" spreadMethod=\"reflect\"><stop stop-color=\"#000\"/><stop offset=\"0.3\" stop-color=\"#fff\"/></linearGradient><linearGradient id=\"h\" href=\"#g\" x2=\"0\"/></defs><rect width=\"30\" height=\"20\" fill=\"url(#g)\"/><rect x=\"32\" width=\"30\" height=\"20\" fill=\"url(#u)\" stroke=\"url(#g)\" stroke-width=\"3\"/><rect y=\"24\" width=\"30\" height=\"20\" fill=\"url(#h)\"/>")),
        ("gradient-radial", w("<defs><radialGradient id=\"r\" cx=\"0.4\" cy=\"0.5\" r=\"0.6\" fx=\"0.2\" fy=\"0.3\"><stop offset=\"0.1\" stop-color=\"yellow\"/><stop offset=\"0.9\" stop-color=\"navy\"/></radialGradient><radialGradient id=\"z\" r=\"0\"><stop stop-color=\"red\"/><stop offset=\"1\" stop-color=\"green\"/></radialGradient></defs><circle cx=\"20\" cy=\"24\" r=\"18\" fill=\"url(#r)\"/><rect x=\"40\" width=\"20\" height=\"20\" fill=\"url(#z)\"/>")),
        ("gradient-edge", w("<defs><linearGradient id=\"one\"><stop stop-color=\"red\" stop-opacity=\"0.5\"/></linearGradient><linearGradient id=\"none\"></linearGradient><linearGradient id=\"dup\"><stop offset=\"0.5\" stop-color=\"red\"/><stop offset=\"0.5\" stop-color=\"lime\"/><stop offset=\"0.5\" stop-color=\"blue\"/><stop offset=\"0.2\" stop-color=\"black\"/></linearGradient></defs><rect width=\"10\" height=\"10\" fill=\"url(#one)\"/><rect x=\"12\" width=\"10\" height=\"10\" fill=\"url(#none) green\"/><rect x=\"24\" width=\"20\" height=\"10\" fill=\"url(#dup)\"/><rect x=\"46\" width=\"10\" height=\"10\" fill=\"url(#missing) currentColor\"/><line x1=\"0\" y1=\"30\" x2=\"60\" y2=\"30\" stroke=\"url(#dup) none\" stroke-width=\"4\"/><line x1=\"0\" y1=\"40\" x2=\"60\" y2=\"40\" stroke=\"url(#dup)\" stroke-width=\"4\"/>")),
        ("pattern", w("<defs><pattern id=\"p\" width=\"0.25\" height=\"0.25\"><rect width=\"4\" height=\"4\" fill=\"red\"/></pattern><pattern id=\"q\" width=\"8\" height=\"8\" patternUnits=\"userSpaceOnUse\" viewBox=\"0 0 4 4\" patternTransform=\"rotate(20)\"><circle cx=\"2\" cy=\"2\" r=\"1.5\"/></pattern></defs><rect width=\"30\" height=\"30\" fill=\"url(#p)\"/><circle cx=\"45\" cy=\"20\" r=\"15\" fill=\"url(#q)\" stroke=\"url(#p)\"/>")),
        ("clip-mask", w("<defs><clipPath id=\"c\"><circle cx=\"15\" cy=\"15\" r=\"10\"/><rect x=\"20\" width=\"5\" height=\"5\" transform=\"rotate(10)\"/></clipPath><clipPath id=\"cb\" clipPathUnits=\"objectBoundingBox\"><rect width=\"0.5\" height=\"1\"/></clipPath><mask id=\"m\"><rect width=\"64\" height=\"48\" fill=\"white\"/><circle cx=\"45\" cy=\"20\" r=\"6\" fill=\"black\"/></mask><mask id=\"ma\" mask-type=\"alpha\" maskContentUnits=\"objectBoundingBox\"><rect width=\"0.5\" height=\"0.5\" fill=\"black\" fill-opacity=\"0.5\"/></mask></defs><rect width=\"30\" height=\"30\" fill=\"blue\" clip-path=\"url(#c)\"/><g mask=\"url(#m)\"><rect x=\"32\" width=\"30\" height=\"40\" fill=\"red\"/></g><rect y=\"32\" width=\"30\" height=\"15\" fill=\"green\" clip-path=\"url(#cb)\"/><rect x=\"35\" y=\"32\" width=\"25\" height=\"15\" fill=\"black\" mask=\"url(#ma)\"/><rect width=\"5\" height=\"5\" clip-path=\"url(#missing)\"/><rect width=\"5\" height=\"5\" mask=\"url(#c)\"/>")),
        ("filter", w("<defs><filter id=\"f\"><feGaussianBlur stdDeviation=\"2\"/></filter><filter id=\"empty\"/></defs><rect x=\"10\" y=\"10\" width=\"20\" height=\"20\" fill=\"red\" filter=\"url(#f)\"/><rect x=\"40\" y=\"10\" width=\"10\" height=\"10\" filter=\"url(#empty)\"/><rect x=\"40\" y=\"30\" width=\"10\" height=\"10\" filter=\"url(#missing)\"/><rect x=\"5\" y=\"35\" width=\"10\" height=\"10\" filter=\"blur(2) grayscale(1)\"/>")),
        ("image", format!("<svg {NS} width=\"40\" height=\"30\"><image id=\"photo\" x=\"2\" y=\"2\" width=\"20\" height=\"20\" href=\"{u}\" style=\"image-rendering:pixelated\"/><image x=\"24\" y=\"4\" width=\"14\" height=\"8\" href=\"{u}\" preserveAspectRatio=\"xMaxYMid slice\"/><image x=\"24\" y=\"14\" width=\"14\" href=\"{u}\"/><image href=\"data:text/plain;base64,AAAA\"/><image href=\"/etc/passwd\" width=\"5\" height=\"5\"/></svg>", u = png_url())),
        ("text", w("<text x=\"5\" y=\"20\" font-size=\"12\">Hello</text><text>   </text><rect width=\"5\" height=\"5\"/>")),
        ("entities", format!("<!DOCTYPE svg [<!ENTITY ns \"http://www.w3.org/2000/svg\"><!ENTITY red \"#ff0000\">]><svg xmlns=\"&ns;\" width=\"20\" height=\"20\"><rect width=\"10\" height=\"10\" fill=\"&red;\"/><rect x=\"10\" width=\"5\" height=\"5\" fill=\"&#x23;00f\"/><![CDATA[ ignored ]]><!-- c --></svg>")),
        ("prefixed", "<s:svg xmlns:s=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\"><s:rect width=\"10\" height=\"10\"/><x:rect xmlns:x=\"urn:other\" width=\"20\" height=\"20\"/></s:svg>".to_string()),
        ("a-link", w("<a href=\"https://example.com\"><rect width=\"10\" height=\"10\"/></a><unknown><rect x=\"20\" width=\"10\" height=\"10\"/></unknown>")),
        ("background", format!("<svg {NS} width=\"20\" height=\"20\" background-color=\"#ff000080\" opacity=\"0.5\" transform=\"translate(1 1)\"><rect width=\"10\" height=\"10\"/></svg>")),
        ("empty", format!("<svg {NS} width=\"8\" height=\"8\"/>")),
        ("huge", format!("<svg {NS} viewBox=\"0 0 100000 50000\"><rect x=\"50000\" width=\"50000\" height=\"50000\" fill=\"black\"/></svg>")),
        ("bad-unclosed", "<svg".to_string()),
        ("bad-rect", format!("<svg {NS}><rect")),
        ("bad-text", "not xml at all".to_string()),
        ("bad-empty", String::new()),
        ("bad-root", "<html xmlns=\"http://www.w3.org/2000/svg\"/>".to_string()),
        ("bad-size", format!("<svg {NS} width=\"0\" height=\"10\"/>")),
        ("bad-utf8", "<svg \u{1}".to_string()),
    ];
    // Many elements: one pixel layer.
    let mut many = String::new();
    for i in 0..2100 {
        many.push_str(&format!("<rect x=\"{}\" y=\"{}\" width=\"1\" height=\"1\"/>", i % 60, i / 60));
    }
    v.push(("many", w(&many)));
    // Groups nested past the layer depth limit are rasterised.
    let deep = format!("{}<rect width=\"10\" height=\"10\" fill=\"red\"/>{}", "<g opacity=\"0.99\">".repeat(110), "</g>".repeat(110));
    v.push(("deep", w(&deep)));
    v.into_iter().map(|(a, b)| (a.to_string(), b)).collect()
}

fn rgba8_b64(b: &photocraft_compose::Buffer) -> String {
    b64(&b.to_rgba8().pixels)
}

fn svg() {
    for (name, text) in svgs() {
        let r = photocraft_io::import(&format!("{name}.svg"), text.as_bytes());
        println!("svg\t{name}\t{}\t{}", b64(text.as_bytes()), imp(&r, false));
        if let Ok(r) = &r {
            if (r.document.size.width as u64) * (r.document.size.height as u64) <= 64 * 64 * 4 {
                let b = photocraft_compose::flatten(&r.document);
                println!("svgpx\t{name}\t{}\t{}\t{}", b.rect.width(), b.rect.height(), rgba8_b64(&b));
            }
        }
    }
    let raster = [
        ("line", format!("<svg {NS} width=\"20\" height=\"10\"><rect x=\"0\" y=\"4\" width=\"20\" height=\"0.5\" fill=\"black\"/></svg>")),
        ("art", format!("<svg {NS} width=\"24\" height=\"16\" viewBox=\"0 0 12 8\"><circle cx=\"4\" cy=\"4\" r=\"3\" fill=\"red\" stroke=\"blue\"/><path d=\"M6 1 L11 7 L6 7 Z\" fill=\"green\" opacity=\"0.5\"/></svg>")),
        ("huge", format!("<svg {NS} width=\"10000\" height=\"10000\"><rect width=\"10\" height=\"10\"/></svg>")),
        ("bad", "<svg".to_string()),
    ];
    for (name, text) in raster {
        for scale in [0.5f32, 1.0, 3.0, 64.0, 0.0, f32::NAN] {
            let res = match photocraft_io::svg::rasterize(text.as_bytes(), scale) {
                Ok(b) if (b.rect.width() as u64) * (b.rect.height() as u64) <= 100 * 100 => format!("{} {} {}", b.rect.width(), b.rect.height(), rgba8_b64(&b)),
                Ok(b) => format!("{} {} -", b.rect.width(), b.rect.height()),
                Err(e) => format!("err {}", sj!(&e.to_string())),
            };
            println!("svgr\t{name}\t{}\t{}\t{res}", b64(text.as_bytes()), fbits(scale));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Corpus composite summaries (REWRITE_PLAN.md section 8, "Corpus composite goldens")
// ---------------------------------------------------------------------------------------------

/// SHA-256 (FIPS 180-4); rustref has no sha2 dependency.
fn sha256(data: &[u8]) -> String {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ];
    let mut h: [u32; 8] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
    let mut msg = data.to_vec();
    let bits = (data.len() as u64).wrapping_mul(8);
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend(bits.to_be_bytes());
    for block in msg.chunks_exact(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([block[4 * i], block[4 * i + 1], block[4 * i + 2], block[4 * i + 3]]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
        }
        let mut v = h;
        for i in 0..64 {
            let s1 = v[4].rotate_right(6) ^ v[4].rotate_right(11) ^ v[4].rotate_right(25);
            let ch = (v[4] & v[5]) ^ (!v[4] & v[6]);
            let t1 = v[7].wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0 = v[0].rotate_right(2) ^ v[0].rotate_right(13) ^ v[0].rotate_right(22);
            let maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            let t2 = s0.wrapping_add(maj);
            v = [t1.wrapping_add(t2), v[0], v[1], v[2], v[3].wrapping_add(t1), v[4], v[5], v[6]];
        }
        for i in 0..8 {
            h[i] = h[i].wrapping_add(v[i]);
        }
    }
    h.iter().map(|x| format!("{x:08x}")).collect()
}

/// Box-averaged thumbnail of straight RGBA8 pixels, min(64, side) per axis.
fn thumbnail(px: &[u8], w: usize, h: usize) -> (usize, usize, Vec<u8>) {
    let (tw, th) = (w.min(64), h.min(64));
    let span = |d: usize, s: usize, n: usize| {
        let a = (d * s / n).min(s - 1);
        (a, ((d + 1) * s).div_ceil(n).clamp(a + 1, s))
    };
    let mut out = Vec::with_capacity(tw * th * 4);
    for ty in 0..th {
        let (y0, y1) = span(ty, h, th);
        for tx in 0..tw {
            let (x0, x1) = span(tx, w, tw);
            let mut sum = [0u64; 4];
            for y in y0..y1 {
                for x in x0..x1 {
                    for c in 0..4 {
                        sum[c] += u64::from(px[(y * w + x) * 4 + c]);
                    }
                }
            }
            let n = ((y1 - y0) * (x1 - x0)) as u64;
            out.extend(sum.map(|s| ((s + n / 2) / n) as u8));
        }
    }
    (tw, th, out)
}

fn composite_summary(path: &std::path::Path, name: &str) -> J {
    let bytes = std::fs::read(path).unwrap();
    let r = match photocraft_io::import(name, &bytes) {
        Ok(r) => r,
        Err(e) => return json!({"error": e.to_string(), "kind": err_kind(&e)}),
    };
    let d = &r.document;
    let buf = photocraft_compose::flatten(d);
    let mut raw = Vec::with_capacity(buf.px.len() * 16);
    let mut stats = [(f64::INFINITY, f64::NEG_INFINITY, 0.0f64, 0u64); 4];
    for p in &buf.px {
        for (c, v) in p.iter().enumerate() {
            let bits = if v.is_nan() { 0x7fc0_0000u32 } else { v.to_bits() };
            raw.extend(bits.to_le_bytes());
            if v.is_finite() {
                let x = f64::from(*v);
                let s = &mut stats[c];
                s.0 = s.0.min(x);
                s.1 = s.1.max(x);
                s.2 += x;
                s.3 += 1;
            }
        }
    }
    let rgba8 = buf.to_rgba8().pixels;
    let (w, h) = (buf.rect.width() as usize, buf.rect.height() as usize);
    let (tw, th, thumb) = thumbnail(&rgba8, w, h);
    use photocraft_codecs::{ChannelLayout, EncodeOptions, Format, Image};
    let png = photocraft_codecs::encode(&Image::from_u8(tw as u32, th as u32, ChannelLayout::Rgba, thumb).unwrap(), Format::Png, &EncodeOptions::default()).unwrap();
    let st: Vec<J> = stats
        .iter()
        .map(|s| if s.3 == 0 { json!(null) } else { json!([s.0, s.1, s.2 / s.3 as f64]) })
        .collect();
    json!({
        "width": d.size.width, "height": d.size.height, "mode": d.mode, "depth": d.depth,
        "rect": [buf.rect.x0, buf.rect.y0, buf.rect.x1, buf.rect.y1],
        "f32_sha256": sha256(&raw),
        "rgba8_sha256": sha256(&rgba8),
        "thumb_png": b64(&png),
        "stats": st,
    })
}

fn composite_goldens(out_dir: &str) {
    let root = concat!(env!("CARGO_MANIFEST_DIR"), "/../../corpus");
    let mut n = 0;
    for corpus in ["psd", "psd-tools", "photoshop", "affinity", "exr", "heif", "pngsuite"] {
        let mut files = Vec::new();
        all_files(&std::path::Path::new(root).join(corpus), &mut files);
        files.sort();
        for p in files {
            let rel = p.strip_prefix(root).unwrap().to_string_lossy().into_owned();
            let name = p.file_name().unwrap().to_string_lossy().into_owned();
            let out = std::path::Path::new(out_dir).join(format!("{rel}.json"));
            std::fs::create_dir_all(out.parent().unwrap()).unwrap();
            std::fs::write(&out, format!("{}\n", composite_summary(&p, &name))).unwrap();
            n += 1;
        }
    }
    eprintln!("composite: {n} summaries under {out_dir}");
}

fn all_files(dir: &std::path::Path, out: &mut Vec<std::path::PathBuf>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        let name = e.file_name().to_string_lossy().into_owned();
        if name.starts_with('.') || name.ends_with(".md") || name.starts_with("LICENSE") {
            continue;
        }
        if p.is_dir() {
            all_files(&p, out);
        } else {
            out.push(p);
        }
    }
}

/// `RUSTREF_COMPOSITE_OUT=DIR rustref io-extra-vectors` writes the corpus composite summaries
/// (one JSON per file, tests/golden/composite/<corpus>/<file>.json) instead of the vectors.
pub fn run() {
    if let Ok(dir) = std::env::var("RUSTREF_COMPOSITE_OUT") {
        composite_goldens(&dir);
        return;
    }
    camera_raw();
    affinity();
    svg();
    svg_i5b();
}

// ---------------------------------------------------------------------------------------------
// I5b: SVG text, filters, markers and nested images (appended after every I5 line so the
// earlier golden lines stay unchanged). Each feature has its own case list.
// ---------------------------------------------------------------------------------------------

fn svg_i5b_lines(cases: Vec<(String, String)>) {
    for (name, text) in cases {
        let r = photocraft_io::import(&format!("{name}.svg"), text.as_bytes());
        println!("svg\t{name}\t{}\t{}", b64(text.as_bytes()), imp(&r, false));
        if let Ok(r) = &r {
            if (r.document.size.width as u64) * (r.document.size.height as u64) <= 64 * 64 * 4 {
                let b = photocraft_compose::flatten(&r.document);
                println!("svgpx\t{name}\t{}\t{}\t{}", b.rect.width(), b.rect.height(), rgba8_b64(&b));
            }
        }
    }
}

/// `<text>` cases (I5b text).
/// Fonts are the ones installed on the machine that generates the golden (the reference's font
/// database is the system's); the C test skips a case whose families are not installed.
fn svg_text_cases() -> Vec<(String, String)> {
    let w = |body: &str| format!("<svg {NS} width=\"64\" height=\"64\">{body}</svg>");
    let v: Vec<(&str, String)> = vec![
        ("text-fonts", w(r##"<text x="2" y="14" font-family="DejaVu Sans" font-size="12">Ag</text><text x="2" y="30" font-family="Liberation Serif" font-size="16">Qy</text><text x="2" y="46" font-family="Courier New" font-size="10">mono</text><text x="30" y="60" font-family="Georgia" font-size="9">g&amp;</text><text x="40" y="14" font-family="Arial" font-size="7px">Ab</text>"##)),
        ("text-anchor", w(r##"<g font-family="DejaVu Serif" font-size="10"><text x="32" y="12">start</text><text x="32" y="28" text-anchor="middle">mid</text><text x="32" y="44" text-anchor="end">end</text><text x="32" y="60" text-anchor="middle">a<tspan text-anchor="end" fill="red">b</tspan></text></g>"##)),
        ("text-tspans", w(r##"<text x="2" y="20" font-family="DejaVu Sans" font-size="10">A<tspan fill="red" font-size="14">b</tspan><tspan font-weight="bold" fill="blue">c</tspan><tspan font-style="italic" fill="green">d</tspan><tspan font-family="Liberation Sans" font-weight="bold" font-style="italic">e</tspan></text><text x="2" y="44" font-family="Liberation Serif" font-size="12" fill="purple"><tspan>x<tspan font-weight="700">y<tspan font-style="italic">z</tspan></tspan></tspan>w</text>"##)),
        ("text-positions", w(r##"<text x="2 10 20" y="12 20" font-family="DejaVu Sans" font-size="9">abcdef</text><text x="4" y="40" dx="0 3 -2 4" dy="0 4 -4" font-family="DejaVu Sans" font-size="9">ghij<tspan dx="5" dy="2">kl</tspan></text><text x="10" y="56" rotate="0 20 -30" font-family="DejaVu Serif" font-size="10">rots</text>"##)),
        ("text-spacing", w(r##"<text x="2" y="14" font-family="Verdana" font-size="8" letter-spacing="2">ab cd</text><text x="2" y="30" font-family="Verdana" font-size="8" word-spacing="6">ab cd ef</text><text x="2" y="46" font-family="Verdana" font-size="8" letter-spacing="-1" word-spacing="-2">ab cd</text><text x="2" y="60" font-family="Verdana" font-size="8">a<tspan letter-spacing="3">bc</tspan>d</text>"##)),
        ("text-baseline", w(r##"<text x="2" y="30" font-family="DejaVu Sans" font-size="10">x<tspan baseline-shift="super">2</tspan>y<tspan baseline-shift="sub">i</tspan>z<tspan baseline-shift="4">u</tspan>w<tspan baseline-shift="-30%">d</tspan><tspan baseline-shift="baseline">b</tspan></text>"##)),
        ("text-dominant", w(r##"<g font-family="Liberation Sans" font-size="9"><text x="2" y="10" dominant-baseline="middle">mid</text><text x="2" y="24" dominant-baseline="hanging">hang</text><text x="2" y="38" dominant-baseline="central">cen</text><text x="2" y="52" alignment-baseline="text-before-edge">tbe</text><text x="34" y="20" dominant-baseline="mathematical">ma<tspan dominant-baseline="no-change">t</tspan></text><text x="34" y="40" alignment-baseline="ideographic">ide</text><text x="34" y="60" dominant-baseline="text-after-edge">tae</text></g>"##)),
        ("text-decoration", w(r##"<g font-family="DejaVu Serif" font-size="10"><text x="2" y="14" text-decoration="underline">under</text><text x="2" y="30" text-decoration="overline" fill="blue">over</text><text x="2" y="46" text-decoration="line-through">thru</text><text x="2" y="60" text-decoration="underline line-through" fill="green">a<tspan fill="red">b</tspan><tspan text-decoration="overline" fill="orange">c</tspan></text></g>"##)),
        ("text-path", w(r##"<defs><path id="p" d="M5 40 Q32 0 60 40"/><path id="c" d="M32 50 m-10 0 a10 10 0 1 0 20 0 a10 10 0 1 0 -20 0 z"/></defs><text font-family="DejaVu Sans" font-size="8"><textPath href="#p" startOffset="5">along the curve</textPath></text><text font-family="DejaVu Sans" font-size="6" fill="blue"><textPath xlink:href="#c" startOffset="50%" xmlns:xlink="http://www.w3.org/1999/xlink">round</textPath></text>"##)),
        ("text-path-misc", w(r##"<defs><path id="l" d="M4 20 L60 20" transform="translate(0 4)"/><rect id="r" x="5" y="40" width="50" height="16"/></defs><text font-family="Liberation Sans" font-size="8">a<textPath href="#l" text-anchor="middle" startOffset="50%">mid<tspan fill="red" dy="-2">X</tspan></textPath>z</text><text font-family="Liberation Sans" font-size="8"><textPath href="#r">rect path text</textPath></text><text x="2" y="62" font-family="Liberation Sans" font-size="8"><textPath href="#missing">gone</textPath>stay</text>"##)),
        ("text-space", w(r##"<text x="2" y="12" font-family="DejaVu Sans Mono" font-size="7" xml:space="preserve">  a   b  </text><text x="2" y="26" font-family="DejaVu Sans Mono" font-size="7">  a
   b	c  </text><text x="2" y="40" font-family="DejaVu Sans Mono" font-size="7">x <tspan> y </tspan> z</text><text x="2" y="54" font-family="DejaVu Sans Mono" font-size="7" xml:space="preserve">p <tspan xml:space="default">  q  </tspan> r</text>"##)),
        ("text-stroke", w(r##"<text x="2" y="24" font-family="DejaVu Sans" font-size="20" font-weight="bold" fill="none" stroke="blue" stroke-width="0.8">Ok</text><text x="2" y="52" font-family="DejaVu Sans" font-size="20" fill="yellow" stroke="black" stroke-width="2" paint-order="stroke">Hi</text>"##)),
        ("text-opacity", w(r##"<rect width="64" height="64" fill="#ccc"/><text x="2" y="20" font-family="DejaVu Sans" font-size="14" fill="red" fill-opacity="0.5">ab<tspan fill-opacity="0.2" fill="blue">cd</tspan></text><text x="2" y="44" font-family="DejaVu Sans" font-size="14" opacity="0.4">ef</text><g opacity="0.6"><text x="2" y="60" font-family="DejaVu Sans" font-size="14">gh</text></g>"##)),
        ("text-gradient", w(r##"<defs><linearGradient id="g"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/></linearGradient><radialGradient id="r" gradientUnits="userSpaceOnUse" cx="32" cy="48" r="20"><stop offset="0" stop-color="yellow"/><stop offset="1" stop-color="green"/></radialGradient></defs><text x="2" y="24" font-family="DejaVu Sans" font-size="20" fill="url(#g)">Grad</text><text x="2" y="56" font-family="DejaVu Sans" font-size="20" fill="url(#r)" stroke="url(#g)" stroke-width="0.5">Rad</text>"##)),
        ("text-clip", w(r##"<clipPath id="cp"><text x="2" y="40" font-family="DejaVu Sans" font-size="36" font-weight="bold">AB</text></clipPath><rect width="64" height="64" fill="navy" clip-path="url(#cp)"/>"##)),
        ("text-mask", w(r##"<mask id="m"><text x="2" y="40" font-family="Liberation Serif" font-size="30" fill="white">MW</text></mask><rect width="64" height="64" fill="maroon" mask="url(#m)"/>"##)),
        ("text-transform", w(r##"<text x="10" y="20" font-family="DejaVu Sans" font-size="10" transform="rotate(15 32 32) scale(1.2)">turn</text><g transform="translate(5 40) skewX(-20)"><text font-family="DejaVu Serif" font-size="12" fill="teal">skew</text></g>"##)),
        ("text-vertical", w(r##"<text x="20" y="4" font-family="DejaVu Sans" font-size="9" writing-mode="tb">ab1</text><text x="44" y="4" font-family="DejaVu Sans" font-size="9" style="writing-mode: vertical-rl" letter-spacing="1">xy<tspan dx="2" fill="red">z</tspan></text>"##)),
        ("text-empty", w(r##"<text>   </text><text/><text x="3" y="10" font-family="DejaVu Sans"><tspan> </tspan></text><text x="3" y="20" font-family="DejaVu Sans" font-size="0">zero</text><text x="3" y="30" font-family="DejaVu Sans" font-size="-4">neg</text><text x="3" y="40" font-family="DejaVu Sans" display="none">hidden</text><text x="3" y="50" font-family="DejaVu Sans" visibility="hidden">vis</text><rect x="50" y="50" width="5" height="5"/>"##)),
        ("text-names", w(r##"<g font-family="DejaVu Sans" font-size="4"><text id="named" x="2" y="6">x</text><text x="2" y="14">  a fairly long sentence that goes past thirty one chars</text><text x="2" y="22">one<tspan x="30">two</tspan></text><text x="2" y="30">é ü</text></g>"##)),
        ("text-weights", w(r##"<g font-family="DejaVu Sans" font-size="9"><text x="2" y="10" font-weight="bolder">bo</text><text x="2" y="22" font-weight="900">nine</text><text x="2" y="34" font-weight="lighter">li</text><text x="2" y="46" font-stretch="condensed">cond</text><text x="2" y="58" font-weight="bold"><tspan font-weight="lighter">x</tspan><tspan font-weight="bolder">y</tspan></text><text x="34" y="10" font-weight="300">thin</text><text x="34" y="22" font-style="oblique">obl</text><text x="34" y="34" font-family="Liberation Mono" font-style="oblique">o</text></g>"##)),
        ("text-length", w(r##"<g font-family="Liberation Serif" font-size="10"><text x="2" y="14" textLength="50">wide</text><text x="2" y="30" textLength="40" lengthAdjust="spacingAndGlyphs">squash</text><text x="2" y="46" textLength="-5">neg</text><text x="2" y="60">a<tspan textLength="30">bc</tspan>d</text></g>"##)),
        ("text-tref", w(r##"<defs><text id="src">ref<tspan>erenced</tspan></text></defs><text x="2" y="20" font-family="DejaVu Sans" font-size="8" fill="green"><tref href="#src"/></text><text x="2" y="40" font-family="DejaVu Sans" font-size="8">[<tref xlink:href="#src" xmlns:xlink="http://www.w3.org/1999/xlink" fill="red"/>]<tref href="#nope"/></text>"##)),
        ("text-shorthand", w(r##"<text x="2" y="16" style="font: italic bold 12px 'DejaVu Serif'">It</text><text x="2" y="36" style="font: 10px/2 Liberation Mono; fill: red">mo</text><text x="2" y="56" style="font: small-caps condensed 600 9px &quot;DejaVu Sans&quot;, serif">Sc</text><text x="34" y="16" style="font: bogus">bad</text>"##)),
        ("text-kerning", w(r##"<g font-family="Liberation Serif" font-size="14"><text x="2" y="16">AVAT</text><text x="2" y="34" kerning="0">AVAT</text><text x="2" y="52" style="font-kerning: none">AVAT</text></g>"##)),
        ("text-ligature", w(r##"<text x="2" y="20" font-family="DejaVu Serif" font-size="14">ffi fl</text><text x="2" y="44" font-family="DejaVu Serif" font-size="14">f<tspan fill="red">f</tspan>i</text><text x="2" y="60" font-family="DejaVu Sans" font-size="10" font-variant="small-caps">Small</text>"##)),
        ("text-units", w(r##"<g font-size="10"><text x="1em" y="2em" font-family="DejaVu Sans" font-size="1.2em">em<tspan dx="0.5em" font-size="50%">pc</tspan></text><text x="2" y="44" font-family="DejaVu Sans" font-size="12pt" dy="1ex">pt</text><text x="2" y="60" font-family="DejaVu Sans" font-size="large">L<tspan font-size="smaller">s</tspan></text></g>"##)),
        ("text-bidi", w(r##"<text x="2" y="20" font-family="DejaVu Sans" font-size="10">abc אבג</text><text x="60" y="40" font-family="DejaVu Sans" font-size="10" text-anchor="end">שלום 12</text><text x="2" y="58" font-family="DejaVu Sans" font-size="10">a<tspan fill="red">ב</tspan>c</text>"##)),
        ("text-marks", w("<text x=\"2\" y=\"20\" font-family=\"DejaVu Sans\" font-size=\"14\">e\u{301}a\u{308}\u{323}</text><text x=\"2\" y=\"44\" font-family=\"DejaVu Serif\" font-size=\"14\">x\u{300}y</text>")),
        ("text-default-font", w(r##"<text x="2" y="20" font-size="12">Times</text><text x="2" y="40" font-family="Times New Roman" font-style="italic" font-weight="bold" font-size="12">TNR</text><text x="2" y="60" font-family="'Arial', 'DejaVu Sans'" font-size="12">Ar</text>"##)),
        ("text-unknown-font", w(r##"<text x="2" y="20" font-family="NoSuchFontXyz" font-size="12">who</text><text x="2" y="44" font-family="NoSuchFontXyz, Verdana" font-size="12">ver</text>"##)),
        ("text-generic", w(r##"<g font-size="10"><text x="2" y="12" font-family="serif">se</text><text x="2" y="26" font-family="sans-serif">sa</text><text x="2" y="40" font-family="monospace">mo</text><text x="2" y="54" font-family="cursive">cu</text><text x="34" y="12" font-family="fantasy">fa</text></g>"##)),
        ("text-in-use", w(r##"<defs><text id="t" font-family="DejaVu Sans" font-size="10">use</text></defs><use href="#t" x="5" y="20"/><use href="#t" x="20" y="45" fill="red"/>"##)),
        ("text-in-pattern", w(r##"<defs><pattern id="pt" width="16" height="16" patternUnits="userSpaceOnUse"><text x="1" y="12" font-family="DejaVu Sans" font-size="12">p</text></pattern></defs><rect width="64" height="64" fill="url(#pt)"/>"##)),
    ];
    v.into_iter().map(|(n, s)| (n.to_string(), s)).collect()
}

/// Filter cases (I5b filters).
fn svg_filter_cases() -> Vec<(String, String)> {
    let w = |body: &str| format!("<svg {NS} xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"64\" height=\"64\">{body}</svg>");
    let png = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFElEQVR4nGP4z8DwHwyBNBAwNAAAR0sIeRPxYNAAAAAASUVORK5CYII=";
    let v: Vec<(&str, String)> = vec![
        // each primitive alone, pixel-aligned input (exact)
        ("filter-blur-box", w(r##"<filter id="f"><feGaussianBlur stdDeviation="3"/></filter><rect x="20" y="20" width="20" height="20" fill="#c03" filter="url(#f)"/>"##)),
        ("filter-blur-iir", w(r##"<filter id="f"><feGaussianBlur stdDeviation="1"/></filter><rect x="20" y="20" width="20" height="20" fill="#3a6" filter="url(#f)"/><rect x="5" y="5" width="10" height="40" fill="blue" filter="url(#f)"/>"##)),
        ("filter-blur-xy", w(r##"<filter id="f" x="-1" y="-1" width="3" height="3"><feGaussianBlur stdDeviation="5 0.5"/></filter><rect x="24" y="10" width="16" height="40" fill="navy" filter="url(#f)"/>"##)),
        ("filter-offset", w(r##"<filter id="f" x="0" y="0" width="2" height="2"><feOffset dx="7" dy="-3.6"/></filter><rect x="10" y="20" width="20" height="20" fill="orange" filter="url(#f)"/>"##)),
        ("filter-flood", w(r##"<filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="64" height="64"><feFlood flood-color="teal" flood-opacity="0.5" x="8" y="12" width="30" height="20"/></filter><rect x="20" y="20" width="20" height="20" filter="url(#f)"/><filter id="g"><feFlood flood-color="rgba(255,0,0,0.5)" style="flood-opacity:0.6"/></filter><rect x="44" y="40" width="10" height="10" filter="url(#g)"/>"##)),
        ("filter-blend", w(r##"<filter id="f"><feFlood flood-color="#08f" result="fl"/><feBlend in="SourceGraphic" in2="fl" mode="multiply"/></filter><filter id="g"><feFlood flood-color="#f80" result="fl"/><feBlend in="SourceGraphic" in2="fl" mode="screen"/></filter><filter id="h"><feFlood flood-color="#8f0" result="fl"/><feBlend in2="fl" mode="difference"/></filter><rect x="5" y="5" width="20" height="20" fill="#c84" filter="url(#f)"/><rect x="35" y="5" width="20" height="20" fill="#48c" filter="url(#g)"/><rect x="20" y="35" width="20" height="20" fill="#c4c" filter="url(#h)"/>"##)),
        ("filter-composite", w(r##"<filter id="a" x="0" y="0" width="1" height="1"><feFlood flood-color="red" flood-opacity="0.6" x="10" y="10" width="20" height="20" result="f"/><feComposite in="SourceGraphic" in2="f" operator="in"/></filter><filter id="b"><feFlood flood-color="red" result="f"/><feComposite in2="f" operator="out"/></filter><filter id="c"><feFlood flood-color="#0f0" flood-opacity="0.5" result="f"/><feComposite in="f" in2="SourceGraphic" operator="atop"/></filter><filter id="d"><feFlood flood-color="blue" result="f"/><feComposite in="f" in2="SourceAlpha" operator="xor"/></filter><filter id="e"><feFlood flood-color="#36c" result="f"/><feComposite in="SourceGraphic" in2="f" operator="arithmetic" k1="0.5" k2="0.5" k3="0.3" k4="0.1"/></filter><rect x="4" y="4" width="20" height="20" fill="blue" filter="url(#a)"/><rect x="34" y="4" width="20" height="20" fill="#963" filter="url(#b)"/><rect x="4" y="34" width="20" height="20" fill="#963" filter="url(#c)"/><rect x="34" y="34" width="20" height="20" fill="#963" filter="url(#d)"/><rect x="26" y="26" width="10" height="10" fill="#c60" fill-opacity="0.7" filter="url(#e)"/>"##)),
        ("filter-merge-shadow", w(r##"<filter id="f" x="-0.5" y="-0.5" width="2" height="2"><feGaussianBlur in="SourceAlpha" stdDeviation="2.5" result="b"/><feOffset in="b" dx="4" dy="4" result="o"/><feMerge><feMergeNode in="o"/><feMergeNode in="SourceGraphic"/></feMerge></filter><rect x="12" y="12" width="30" height="24" fill="gold" filter="url(#f)"/>"##)),
        ("filter-glow", w(r##"<filter id="f" x="-1" y="-1" width="3" height="3"><feMorphology operator="dilate" radius="2" in="SourceAlpha"/><feGaussianBlur stdDeviation="2"/><feFlood flood-color="cyan"/><feComposite in2="result2" operator="in"/><feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge></filter><rect x="20" y="20" width="24" height="24" fill="#025" filter="url(#f)"/>"##)),
        ("filter-colormatrix", w(r##"<filter id="m"><feColorMatrix type="matrix" values="0 1 0 0 0  0 0 1 0 0  1 0 0 0 0  0 0 0 0.7 0.1"/></filter><filter id="s"><feColorMatrix type="saturate" values="0.3"/></filter><filter id="h"><feColorMatrix type="hueRotate" values="120"/></filter><filter id="l"><feColorMatrix type="luminanceToAlpha"/></filter><filter id="bad"><feColorMatrix type="matrix" values="1 2 3"/></filter><rect x="4" y="4" width="20" height="20" fill="#c73" filter="url(#m)"/><rect x="34" y="4" width="20" height="20" fill="#c73" filter="url(#s)"/><rect x="4" y="34" width="20" height="20" fill="#c73" filter="url(#h)"/><rect x="34" y="34" width="20" height="20" fill="#c73" filter="url(#l)"/><rect x="26" y="26" width="10" height="10" fill="#4a8" filter="url(#bad)"/>"##)),
        ("filter-transfer", w(r##"<filter id="t"><feComponentTransfer><feFuncR type="table" tableValues="1 0.5 0"/><feFuncG type="discrete" tableValues="0 0.3 0.6 1"/><feFuncB type="linear" slope="0.5" intercept="0.25"/><feFuncA type="gamma" amplitude="0.9" exponent="2" offset="0.05"/></feComponentTransfer></filter><filter id="u" color-interpolation-filters="sRGB"><feComponentTransfer><feFuncR type="gamma" amplitude="2" exponent="0.5"/><feFuncG type="table"/><feFuncB type="identity"/></feComponentTransfer></filter><rect x="4" y="4" width="26" height="26" fill="#9c4" filter="url(#t)"/><rect x="34" y="34" width="26" height="26" fill="#47a" fill-opacity="0.8" filter="url(#u)"/>"##)),
        ("filter-morphology", w(r##"<filter id="e"><feMorphology operator="erode" radius="2 1"/></filter><filter id="d" x="-0.5" y="-0.5" width="2" height="2"><feMorphology operator="dilate" radius="3"/></filter><rect x="4" y="4" width="22" height="22" fill="#a30" filter="url(#e)"/><path d="M40 10h14v4h-10v8h-4z" fill="#03a" filter="url(#d)"/>"##)),
        ("filter-convolve", w(r##"<filter id="e"><feConvolveMatrix order="3" kernelMatrix="-2 -1 0 -1 1 1 0 1 2"/></filter><filter id="b"><feConvolveMatrix order="3 2" kernelMatrix="1 2 1 1 2 1" divisor="6" bias="0.1" edgeMode="wrap"/></filter><filter id="p"><feConvolveMatrix order="3" kernelMatrix="0 -1 0 -1 5 -1 0 -1 0" preserveAlpha="true" edgeMode="none" targetX="0"/></filter><rect x="4" y="4" width="24" height="24" fill="#c63" filter="url(#e)"/><rect x="34" y="4" width="24" height="24" fill="#3c6" fill-opacity="0.6" filter="url(#b)"/><rect x="18" y="34" width="24" height="24" fill="#63c" filter="url(#p)"/>"##)),
        ("filter-turbulence", w(r##"<filter id="t" filterUnits="userSpaceOnUse" x="0" y="0" width="32" height="32"><feTurbulence baseFrequency="0.08" numOctaves="2" seed="3"/></filter><filter id="n" filterUnits="userSpaceOnUse" x="32" y="0" width="32" height="32"><feTurbulence type="fractalNoise" baseFrequency="0.1 0.05" numOctaves="3" stitchTiles="stitch"/></filter><filter id="s" filterUnits="userSpaceOnUse" x="0" y="32" width="64" height="32" color-interpolation-filters="sRGB"><feTurbulence baseFrequency="0.2" seed="-7"/></filter><rect width="10" height="10" filter="url(#t)"/><rect x="40" width="10" height="10" filter="url(#n)"/><rect y="40" width="10" height="10" filter="url(#s)"/>"##)),
        ("filter-image", w(&format!(r##"<defs><circle id="c" cx="8" cy="8" r="6" fill="purple"/></defs><filter id="r"><feImage xlink:href="#c"/></filter><filter id="d"><feImage href="{png}" preserveAspectRatio="none"/></filter><filter id="p"><feImage href="{png}" image-rendering="pixelated"/></filter><rect x="4" y="4" width="24" height="24" fill="red" filter="url(#r)"/><rect x="34" y="4" width="24" height="24" filter="url(#d)"/><rect x="4" y="34" width="24" height="20" filter="url(#p)"/><filter id="m"><feImage href="#missing"/></filter><rect x="40" y="40" width="10" height="10" fill="lime" filter="url(#m)"/>"##))),
        ("filter-tile", w(r##"<filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="64" height="64"><feFlood flood-color="#c39" x="4" y="4" width="8" height="6" result="a"/><feOffset in="a" dx="3" dy="3" result="b"/><feMerge result="c" x="2" y="2" width="14" height="12"><feMergeNode in="a"/><feMergeNode in="SourceGraphic"/></feMerge><feTile in="c"/></filter><rect x="6" y="6" width="4" height="4" fill="#123" filter="url(#f)"/>"##)),
        ("filter-displacement", w(r##"<filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="64" height="64"><feTurbulence baseFrequency="0.05" result="t"/><feDisplacementMap in="SourceGraphic" in2="t" scale="10" xChannelSelector="R" yChannelSelector="G"/></filter><rect x="10" y="10" width="40" height="40" fill="#284" filter="url(#f)"/>"##)),
        ("filter-lighting", w(r##"<filter id="d"><feDiffuseLighting surfaceScale="3" diffuseConstant="1.2" lighting-color="#fc8"><feDistantLight azimuth="45" elevation="40"/></feDiffuseLighting></filter><filter id="p"><feDiffuseLighting surfaceScale="2"><fePointLight x="20" y="10" z="15"/></feDiffuseLighting></filter><filter id="s"><feSpecularLighting surfaceScale="4" specularConstant="1" specularExponent="12" lighting-color="white"><feSpotLight x="10" y="40" z="30" pointsAtX="20" pointsAtY="50" pointsAtZ="0" specularExponent="2" limitingConeAngle="40"/></feSpecularLighting><feComposite in2="SourceAlpha" operator="in"/></filter><filter id="q"><feSpecularLighting specularExponent="200"><feDistantLight/></feSpecularLighting></filter><rect x="4" y="4" width="24" height="24" fill="#888" filter="url(#d)"/><circle cx="46" cy="16" r="12" fill="#888" filter="url(#p)"/><rect x="4" y="34" width="26" height="26" rx="6" fill="#888" filter="url(#s)"/><rect x="40" y="40" width="16" height="16" fill="#888" filter="url(#q)"/>"##)),
        ("filter-dropshadow", w(r##"<filter id="f" x="-0.5" y="-0.5" width="2" height="2"><feDropShadow dx="3" dy="2" stdDeviation="2" flood-color="#306" flood-opacity="0.7"/></filter><filter id="g" x="-0.5" y="-0.5" width="2" height="2" color-interpolation-filters="sRGB"><feDropShadow stdDeviation="0.8"/></filter><rect x="10" y="10" width="20" height="20" fill="#9cf" filter="url(#f)"/><rect x="38" y="36" width="16" height="16" fill="#fc9" filter="url(#g)"/>"##)),
        ("filter-colorspace", w(r##"<filter id="l"><feGaussianBlur stdDeviation="3"/></filter><filter id="s" color-interpolation-filters="sRGB"><feGaussianBlur stdDeviation="3"/></filter><g color-interpolation-filters="sRGB"><filter id="i"><feGaussianBlur stdDeviation="3"/><feOffset dx="1" color-interpolation-filters="linearRGB"/></filter></g><rect x="6" y="6" width="20" height="20" fill="#e21" filter="url(#l)"/><rect x="36" y="6" width="20" height="20" fill="#e21" filter="url(#s)"/><rect x="20" y="36" width="20" height="20" fill="#2e1" filter="url(#i)"/>"##)),
        ("filter-units", w(r##"<filter id="u" filterUnits="userSpaceOnUse" x="2" y="2" width="30" height="30"><feGaussianBlur stdDeviation="2"/></filter><filter id="o" primitiveUnits="objectBoundingBox"><feOffset dx="0.25" dy="0.1"/><feGaussianBlur stdDeviation="0.05 0.1"/></filter><filter id="pr" primitiveUnits="objectBoundingBox"><feFlood flood-color="green" x="0.25" y="0.25" width="0.5" height="0.5"/></filter><rect x="6" y="6" width="22" height="22" fill="#a2c" filter="url(#u)"/><rect x="36" y="6" width="20" height="20" fill="#2ac" filter="url(#o)"/><rect x="20" y="36" width="24" height="24" fill="#ca2" filter="url(#pr)"/>"##)),
        ("filter-subregion", w(r##"<filter id="f" x="-0.2" y="-0.2" width="1.4" height="1.4"><feGaussianBlur stdDeviation="3" x="10" y="10" width="20" height="30" result="b"/><feFlood flood-color="#036" flood-opacity="0.4" y="40" height="10"/><feMerge><feMergeNode in="b"/><feMergeNode/></feMerge></filter><rect x="10" y="10" width="40" height="40" fill="#f60" filter="url(#f)"/>"##)),
        ("filter-graph", w(r##"<filter id="f" x="-0.3" y="-0.3" width="1.6" height="1.6"><feOffset dx="-3" result="r"/><feOffset in="r" dy="5" result="r"/><feFlood flood-color="red" result="red"/><feComposite in="red" in2="r" operator="in" result="x"/><feBlend in="nosuch" in2="SourceGraphic" mode="darken"/><feMerge><feMergeNode in="x"/><feMergeNode in="BackgroundImage"/><feMergeNode in="SourceAlpha"/></feMerge></filter><rect x="16" y="16" width="30" height="30" fill="#4ad" fill-opacity="0.5" filter="url(#f)"/>"##)),
        ("filter-emboss", w(r##"<filter id="f"><feGaussianBlur in="SourceAlpha" stdDeviation="1.5" result="b"/><feSpecularLighting in="b" surfaceScale="5" specularConstant="0.8" specularExponent="20" lighting-color="#fff" result="s"><fePointLight x="-20" y="-40" z="80"/></feSpecularLighting><feComposite in="s" in2="SourceAlpha" operator="in" result="s2"/><feComposite in="SourceGraphic" in2="s2" operator="arithmetic" k2="1" k3="1"/></filter><rect x="12" y="12" width="40" height="40" rx="8" fill="#b52" filter="url(#f)"/>"##)),
        ("filter-css", w(r##"<rect x="2" y="2" width="16" height="16" fill="#c52" filter="blur(1.5px)"/><rect x="24" y="2" width="16" height="16" fill="#c52" filter="drop-shadow(2px 3px 1px rgba(0,0,80,0.6))"/><rect x="46" y="2" width="16" height="16" fill="#c52" filter="grayscale(60%)"/><rect x="2" y="24" width="16" height="16" fill="#c52" filter="sepia(1)"/><rect x="24" y="24" width="16" height="16" fill="#c52" filter="saturate(250%)"/><rect x="46" y="24" width="16" height="16" fill="#c52" filter="hue-rotate(0.5turn)"/><rect x="2" y="46" width="16" height="16" fill="#c52" filter="invert(0.8) opacity(70%)"/><rect x="24" y="46" width="16" height="16" fill="#c52" filter="brightness(1.6)"/><rect x="46" y="46" width="16" height="16" fill="#c52" color="green" filter="contrast(40%) drop-shadow(currentColor -2px -2px)"/>"##)),
        ("filter-css-bad", w(r##"<rect x="2" y="2" width="20" height="20" fill="#2c5" filter="blur(2) , grayscale(1)"/><rect x="30" y="2" width="20" height="20" fill="#2c5" filter="blur(-2)"/><rect x="2" y="30" width="20" height="20" fill="#2c5" filter="drop-shadow()"/><rect x="30" y="30" width="20" height="20" fill="#2c5" filter="none"/><rect x="40" y="50" width="10" height="10" fill="#2c5" filter="hue-rotate(30)"/>"##)),
        ("filter-multiple", w(r##"<filter id="a"><feOffset dx="4" dy="4"/></filter><filter id="b" x="-0.5" y="-0.5" width="2" height="2"><feGaussianBlur stdDeviation="1.5"/></filter><rect x="8" y="8" width="20" height="20" fill="#36c" filter="url(#a) url(#b)"/><rect x="36" y="8" width="20" height="20" fill="#36c" filter="url(#b) grayscale(1) url(#a)"/><rect x="20" y="38" width="20" height="20" fill="#36c" filter="url(#missing) url(#a)"/>"##)),
        ("filter-transformed", w(r##"<filter id="f"><feGaussianBlur stdDeviation="2"/><feOffset dx="3"/></filter><g transform="rotate(30 32 32)" filter="url(#f)"><rect x="20" y="20" width="24" height="24" fill="#a4c"/></g><g transform="translate(4 40) scale(1.5 0.75)"><rect width="16" height="16" fill="#4ca" filter="url(#f)"/></g>"##)),
        ("filter-edge", w(r##"<filter id="f" x="-1" y="-1" width="3" height="3"><feGaussianBlur stdDeviation="4"/></filter><rect x="-10" y="-10" width="20" height="20" fill="#c22" filter="url(#f)"/><rect x="54" y="50" width="20" height="20" fill="#22c" filter="url(#f)"/><filter id="g" filterUnits="userSpaceOnUse" x="-500" y="-500" width="2000" height="2000"><feFlood flood-color="#ff0" flood-opacity="0.3"/></filter><rect x="20" y="20" width="10" height="10" filter="url(#g)"/>"##)),
        ("filter-invalid", w(r##"<rect x="2" y="2" width="20" height="20" fill="red" filter="url(#missing)"/><rect x="30" y="2" width="20" height="20" fill="red" filter="url(#notfilter)"/><linearGradient id="notfilter"/><filter id="empty"/><rect x="2" y="30" width="20" height="20" fill="red" filter="url(#empty)"/><filter id="zero" width="0"><feFlood/></filter><rect x="30" y="30" width="20" height="20" fill="red" filter="url(#zero)"/><filter id="ok"><feOffset dx="1"/></filter><line x1="10" y1="56" x2="50" y2="56" stroke="blue" stroke-width="4" filter="url(#ok)"/><filter id="nop"><feFuncR/><desc/></filter><rect x="54" y="54" width="8" height="8" fill="red" filter="url(#nop)"/>"##)),
        ("filter-href", w(r##"<filter id="base" x="-0.5" y="-0.5" width="2" height="2"><feGaussianBlur stdDeviation="2"/><feOffset dx="2" dy="2"/></filter><filter id="child" xlink:href="#base" filterUnits="userSpaceOnUse" x="20" y="20" width="40" height="40"/><filter id="own" xlink:href="#base"><feFlood flood-color="#0c9" flood-opacity="0.5"/></filter><rect x="4" y="4" width="20" height="20" fill="#963" filter="url(#base)"/><rect x="30" y="30" width="20" height="20" fill="#963" filter="url(#child)"/><rect x="40" y="4" width="16" height="16" fill="#963" filter="url(#own)"/>"##)),
        ("filter-empty-group", w(r##"<filter id="f" filterUnits="userSpaceOnUse" x="10" y="10" width="30" height="20"><feFlood flood-color="#57c"/></filter><g id="eg" filter="url(#f)"/><filter id="o"><feFlood flood-color="red"/></filter><g id="og" filter="url(#o)"/><g filter="url(#f)"><rect x="30" y="40" width="0" height="10"/></g>"##)),
        ("filter-nested", w(r##"<filter id="b"><feGaussianBlur stdDeviation="1.5"/></filter><pattern id="pt" width="16" height="16" patternUnits="userSpaceOnUse"><rect x="4" y="4" width="8" height="8" fill="#c36" filter="url(#b)"/></pattern><rect width="32" height="32" fill="url(#pt)"/><mask id="m"><rect x="36" y="4" width="24" height="24" fill="white" filter="url(#b)"/></mask><rect x="32" width="32" height="32" fill="#36c" mask="url(#m)"/><clipPath id="c"><rect x="4" y="36" width="24" height="24" filter="url(#b)"/></clipPath><rect y="32" width="32" height="32" fill="#3c6" clip-path="url(#c)"/><svg x="32" y="32" width="32" height="32" viewBox="0 0 16 16"><rect x="4" y="4" width="8" height="8" fill="#963" filter="url(#b)"/></svg>"##)),
        ("filter-use", w(r##"<filter id="b" x="-0.5" y="-0.5" width="2" height="2"><feGaussianBlur stdDeviation="2"/></filter><defs><rect id="r" width="16" height="16" fill="#c90"/><g id="gr" filter="url(#b)"><rect width="16" height="16" fill="#09c"/></g></defs><use href="#r" x="8" y="8" filter="url(#b)"/><use href="#gr" x="38" y="38"/>"##)),
        ("filter-fe-image-loop", w(r##"<filter id="f"><feImage xlink:href="#r"/></filter><rect id="r" x="10" y="10" width="20" height="20" fill="green" filter="url(#f)"/><filter id="g"><feImage xlink:href="#s"/><feOffset dx="2"/></filter><rect id="q" x="36" y="10" width="20" height="20" fill="blue" filter="url(#g)"/><rect id="s" x="36" y="36" width="20" height="20" fill="red" opacity="0.5"/>"##)),
        ("filter-root", format!(r##"<svg {NS} width="48" height="48" filter="url(#f)"><filter id="f"><feGaussianBlur stdDeviation="1"/></filter><rect x="8" y="8" width="32" height="32" fill="#7a3"/></svg>"##)),
        ("filter-pixel-exact", w(r##"<filter id="f" filterUnits="userSpaceOnUse" x="8" y="8" width="48" height="48"><feGaussianBlur stdDeviation="2.5" result="a"/><feColorMatrix type="saturate" values="0.5" result="b"/><feComponentTransfer in="b"><feFuncA type="linear" slope="0.8"/></feComponentTransfer><feMorphology operator="dilate" radius="1"/><feOffset dx="2" dy="1"/></filter><g filter="url(#f)"><rect x="16" y="16" width="16" height="16" fill="#e44"/><rect x="28" y="28" width="16" height="16" fill="#44e" fill-opacity="0.6"/></g>"##)),
    ];
    v.into_iter().map(|(n, s)| (n.to_string(), s)).collect()
}

/// Marker cases (I5b markers).
fn svg_marker_cases() -> Vec<(String, String)> {
    let w = |body: &str| format!("<svg {NS} width=\"64\" height=\"48\">{body}</svg>");
    let tri = "<marker id=\"t\" markerWidth=\"6\" markerHeight=\"6\" refX=\"3\" refY=\"3\" orient=\"auto\"><path d=\"M0 0 L6 3 L0 6 Z\" fill=\"blue\"/></marker>";
    let v: Vec<(&str, String)> = vec![
        ("marker-basic", w(&format!("<defs>{tri}<marker id=\"d\" markerWidth=\"4\" markerHeight=\"4\" refX=\"2\" refY=\"2\"><circle cx=\"2\" cy=\"2\" r=\"2\" fill=\"red\"/></marker></defs><polyline points=\"5,40 20,10 35,40 58,12\" fill=\"none\" stroke=\"black\" marker-start=\"url(#d)\" marker-mid=\"url(#d)\" marker-end=\"url(#t)\"/>"))),
        ("marker-units", w(&format!("<defs>{tri}<marker id=\"u\" markerUnits=\"userSpaceOnUse\" markerWidth=\"8\" markerHeight=\"8\" refX=\"4\" refY=\"4\"><rect width=\"8\" height=\"8\" fill=\"green\"/></marker></defs><path d=\"M5 10 H58\" stroke=\"black\" stroke-width=\"2\" marker-end=\"url(#t)\" marker-start=\"url(#u)\"/><path d=\"M5 30 H58\" stroke=\"black\" stroke-width=\"0.5\" marker-end=\"url(#t)\"/><path d=\"M5 42 H40\" stroke-width=\"bogus\" stroke=\"gray\" marker-end=\"url(#t)\"/>"))),
        ("marker-viewbox", w("<defs><marker id=\"v\" viewBox=\"0 0 10 10\" refX=\"5\" refY=\"5\" markerWidth=\"5\" markerHeight=\"3\" preserveAspectRatio=\"xMinYMid meet\"><circle cx=\"5\" cy=\"5\" r=\"6\" fill=\"purple\"/></marker><marker id=\"o\" viewBox=\"0 0 10 10\" refX=\"5\" refY=\"5\" markerWidth=\"4\" markerHeight=\"4\" overflow=\"visible\" preserveAspectRatio=\"none\"><circle cx=\"5\" cy=\"5\" r=\"7\" fill=\"orange\"/></marker></defs><path d=\"M8 12 L56 12\" stroke=\"black\" marker-start=\"url(#v)\" marker-end=\"url(#o)\"/><path d=\"M8 36 L56 30\" stroke=\"black\" stroke-width=\"2\" marker-start=\"url(#o)\" marker-end=\"url(#v)\"/>")),
        ("marker-orient", w(&format!("<defs>{tri}<marker id=\"a\" markerWidth=\"6\" markerHeight=\"6\" refX=\"3\" refY=\"3\" orient=\"45\"><path d=\"M0 0 L6 3 L0 6 Z\" fill=\"red\"/></marker><marker id=\"r\" markerWidth=\"6\" markerHeight=\"6\" refX=\"3\" refY=\"3\" orient=\"auto-start-reverse\"><path d=\"M0 0 L6 3 L0 6 Z\" fill=\"green\"/></marker><marker id=\"q\" markerWidth=\"6\" markerHeight=\"6\" refX=\"3\" refY=\"3\" orient=\"0.5rad\"><path d=\"M0 0 L6 3 L0 6 Z\" fill=\"teal\"/></marker></defs><path d=\"M5 20 C15 0 25 40 35 20 S50 5 58 18\" fill=\"none\" stroke=\"black\" marker-start=\"url(#r)\" marker-mid=\"url(#t)\" marker-end=\"url(#r)\"/><path d=\"M5 40 Q20 25 35 40 T58 38\" fill=\"none\" stroke=\"gray\" marker-mid=\"url(#a)\" marker-end=\"url(#q)\"/><path d=\"M40 5 L55 5 L55 15 Z\" fill=\"none\" stroke=\"black\" marker-start=\"url(#t)\" marker-mid=\"url(#t)\" marker-end=\"url(#t)\"/>"))),
        ("marker-context", w("<defs><marker id=\"c\" markerWidth=\"8\" markerHeight=\"8\" refX=\"4\" refY=\"4\"><rect width=\"8\" height=\"8\" fill=\"context-stroke\" stroke=\"context-fill\"/></marker><linearGradient id=\"g\"><stop stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient></defs><path d=\"M8 10 L56 10 L56 40\" fill=\"yellow\" stroke=\"navy\" stroke-width=\"1\" marker-start=\"url(#c)\" marker-mid=\"url(#c)\" marker-end=\"url(#c)\"/><path d=\"M8 40 L30 30\" fill=\"none\" stroke=\"url(#g)\" marker-end=\"url(#c)\"/>")),
        ("marker-paint-order", w(&format!("<defs>{tri}</defs><path id=\"p1\" d=\"M5 10 L58 10\" stroke=\"orange\" stroke-width=\"4\" marker-end=\"url(#t)\" paint-order=\"markers\"/><path id=\"p2\" d=\"M5 25 L50 25 L58 40 Z\" fill=\"pink\" stroke=\"black\" stroke-width=\"3\" marker-mid=\"url(#t)\" paint-order=\"stroke markers fill\"/><path d=\"M5 44 L58 44\" stroke=\"red\" stroke-width=\"2\" marker-start=\"url(#t)\" paint-order=\"fill markers\"/>"))),
        ("marker-shapes", w(&format!("<defs>{tri}<marker id=\"self\" markerWidth=\"6\" markerHeight=\"6\" refX=\"3\" refY=\"3\"><g id=\"inner\"><path d=\"M0 0 L6 6\" stroke=\"black\" marker-end=\"url(#self)\"/><rect id=\"rr\" width=\"3\" height=\"3\" fill=\"red\"/></g></marker></defs><rect x=\"5\" y=\"5\" width=\"20\" height=\"10\" fill=\"none\" stroke=\"black\" marker-start=\"url(#t)\" marker-mid=\"url(#t)\" marker-end=\"url(#t)\"/><line x1=\"30\" y1=\"10\" x2=\"58\" y2=\"10\" stroke=\"black\" style=\"marker: url(#t)\"/><polygon points=\"10,25 25,40 5,40\" fill=\"none\" stroke=\"black\" marker-mid=\"url(#self)\"/><circle cx=\"45\" cy=\"32\" r=\"8\" fill=\"none\" stroke=\"blue\" marker-start=\"url(#t)\" marker-end=\"url(#t)\"/><g marker-end=\"url(#t)\"><path d=\"M30 44 L58 44\" stroke=\"gray\"/></g>"))),
        ("marker-edge", w(&format!("<defs>{tri}<marker id=\"e\"/><clipPath id=\"cp\"><path d=\"M0 0 L60 40\" marker-end=\"url(#t)\" stroke=\"black\"/><rect width=\"30\" height=\"30\"/></clipPath></defs><path d=\"M5 5 L20 5\" stroke=\"black\" marker-end=\"url(#e)\"/><path d=\"M5 10 L20 10\" stroke=\"black\" marker-end=\"url(#missing)\" marker-start=\"url(#cp)\"/><rect width=\"40\" height=\"40\" fill=\"green\" clip-path=\"url(#cp)\"/><path d=\"M30 30\" marker-start=\"url(#t)\" stroke=\"black\"/><path d=\"M40 20 L50 20\" visibility=\"hidden\" stroke=\"black\" marker-end=\"url(#t)\"/><path d=\"M40 30 L50 30\" stroke=\"black\" marker-end=\"none\"/><path d=\"M40 40 L50 40\" fill=\"none\" marker-end=\"url(#t)\"/>"))),
    ];
    v.into_iter().map(|(a, b)| (a.to_string(), b)).collect()
}

/// An encoded test image (a 6 x 4 gradient with an alpha ramp) as a data URL.
fn img_url(fmt: photocraft_codecs::Format, mime: &str, alpha: bool) -> String {
    use photocraft_codecs::{ChannelLayout, EncodeOptions, Image};
    let (w, h) = (6u32, 4u32);
    let mut px = Vec::new();
    for y in 0..h {
        for x in 0..w {
            px.extend_from_slice(&[(x * 40) as u8, (y * 60) as u8, 200 - (x * 20) as u8]);
            if alpha {
                px.push(255 - (x * 30 + y * 10) as u8);
            }
        }
    }
    let layout = if alpha { ChannelLayout::Rgba } else { ChannelLayout::Rgb };
    let img = Image::from_u8(w, h, layout, px).unwrap();
    let mut opt = EncodeOptions::default();
    opt.webp_lossless = true;
    format!("data:{mime};base64,{}", b64(&photocraft_codecs::encode(&img, fmt, &opt).unwrap()))
}

/// Nested image cases (I5b images).
fn svg_image_cases() -> Vec<(String, String)> {
    use photocraft_codecs::Format;
    let w = |body: &str| format!("<svg {NS} width=\"64\" height=\"48\">{body}</svg>");
    let nested = format!("<svg {NS} width=\"20\" height=\"10\" viewBox=\"0 0 40 20\"><rect width=\"40\" height=\"20\" fill=\"#08f\"/><circle cx=\"20\" cy=\"10\" r=\"8\" fill=\"yellow\" stroke=\"red\" stroke-width=\"2\"/><image width=\"10\" height=\"10\" href=\"{}\"/></svg>", img_url(Format::Png, "image/png", false));
    let nested_b64 = format!("data:image/svg+xml;base64,{}", b64(nested.as_bytes()));
    let nested_pct = format!("data:image/svg+xml,{}", nested.replace('%', "%25").replace('#', "%23").replace('"', "'").replace('<', "%3C").replace('>', "%3E"));
    let plain = format!("data:text/plain;base64,{}", b64(format!("<svg {NS} width=\"8\" height=\"8\"><rect width=\"8\" height=\"8\" fill=\"green\"/></svg>").as_bytes()));
    let svgz = {
        let mut gz = Vec::new();
        {
            use std::io::Write;
            let mut e = flate2_like_gzip(&mut gz);
            e.write_all(format!("<svg {NS} width=\"8\" height=\"8\"><circle cx=\"4\" cy=\"4\" r=\"4\" fill=\"purple\"/></svg>").as_bytes()).unwrap();
        }
        format!("data:image/svg+xml;base64,{}", b64(&gz))
    };
    let png = img_url(Format::Png, "image/png", true);
    let v: Vec<(&str, String)> = vec![
        ("image-nested-svg", w(&format!("<image x=\"2\" y=\"2\" width=\"40\" height=\"20\" href=\"{nested_b64}\"/><image x=\"2\" y=\"26\" width=\"30\" height=\"20\" preserveAspectRatio=\"xMaxYMax slice\" href=\"{nested_b64}\"/><image id=\"pct\" x=\"40\" y=\"30\" href=\"{nested_pct}\"/>"))),
        ("image-nested-misc", w(&format!("<image x=\"2\" y=\"2\" href=\"{plain}\"/><image x=\"12\" y=\"2\" width=\"16\" href=\"{svgz}\"/><image x=\"30\" y=\"2\" width=\"10\" height=\"10\" href=\"data:image/svg+xml;base64,PHN2Zw==\"/><g opacity=\"0.5\"><image x=\"40\" y=\"20\" width=\"20\" height=\"10\" preserveAspectRatio=\"none\" href=\"{nested_b64}\"/></g><image x=\"2\" y=\"30\" width=\"10\" height=\"10\" href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg'%3E%3Crect width='100' height='50'/%3E%3C/svg%3E\"/>"))),
        ("image-formats", w(&format!("<image x=\"1\" y=\"1\" width=\"12\" height=\"8\" href=\"{}\"/><image x=\"15\" y=\"1\" width=\"12\" height=\"8\" href=\"{png}\"/><image x=\"29\" y=\"1\" width=\"12\" height=\"8\" href=\"{}\"/><image x=\"43\" y=\"1\" width=\"12\" height=\"8\" href=\"{}\"/><image x=\"1\" y=\"12\" width=\"12\" height=\"8\" href=\"{}\"/>",
            img_url(Format::Png, "image/png", false), img_url(Format::Gif, "image/gif", false), img_url(Format::WebP, "image/webp", true), img_url(Format::Png, "text/plain", true)))),
        ("image-rendering", w(&format!("<image x=\"1\" y=\"1\" width=\"30\" height=\"20\" href=\"{png}\"/><image x=\"33\" y=\"1\" width=\"30\" height=\"20\" image-rendering=\"smooth\" href=\"{png}\"/><image x=\"1\" y=\"24\" width=\"30\" height=\"20\" style=\"image-rendering:pixelated\" href=\"{png}\"/><g image-rendering=\"optimizeSpeed\" transform=\"rotate(10 48 34)\"><image x=\"33\" y=\"24\" width=\"30\" height=\"20\" href=\"{png}\"/></g>"))),
        ("image-broken", w(&format!("<image x=\"1\" y=\"1\" width=\"10\" height=\"10\" href=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAQAAAAEAAAAAA==\"/><image x=\"12\" y=\"1\" width=\"10\" height=\"10\" href=\"{}\"/><image x=\"24\" y=\"1\" href=\"data:image/jpeg;base64,AAAA\"/><image x=\"36\" y=\"1\" width=\"10\" height=\"10\" href=\"data:image/bmp;base64,Qk0=\"/><image x=\"1\" y=\"20\" width=\"20\" height=\"20\" href=\"file:///etc/passwd\"/><image width=\"0\" height=\"5\" href=\"{png}\"/>",
            img_url(Format::Jpeg, "image/png", false)))),
    ];
    v.into_iter().map(|(a, b)| (a.to_string(), b)).collect()
}

/// A minimal gzip writer (stored deflate blocks) so rustref needs no compression crate.
fn flate2_like_gzip(out: &mut Vec<u8>) -> GzStore<'_> {
    GzStore { out, buf: Vec::new() }
}

struct GzStore<'a> {
    out: &'a mut Vec<u8>,
    buf: Vec<u8>,
}

impl std::io::Write for GzStore<'_> {
    fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
        self.buf.extend_from_slice(b);
        Ok(b.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl Drop for GzStore<'_> {
    fn drop(&mut self) {
        let d = &self.buf;
        self.out.extend_from_slice(&[0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255]);
        let chunks: Vec<&[u8]> = if d.is_empty() { vec![&[][..]] } else { d.chunks(65535).collect() };
        for (i, c) in chunks.iter().enumerate() {
            self.out.push(if i + 1 == chunks.len() { 1 } else { 0 });
            let n = c.len() as u16;
            self.out.extend_from_slice(&n.to_le_bytes());
            self.out.extend_from_slice(&(!n).to_le_bytes());
            self.out.extend_from_slice(c);
        }
        self.out.extend_from_slice(&crc32(d).to_le_bytes());
        self.out.extend_from_slice(&(d.len() as u32).to_le_bytes());
    }
}

fn svg_i5b() {
    svg_i5b_lines(svg_text_cases());
    svg_i5b_lines(svg_filter_cases());
    svg_i5b_lines(svg_marker_cases());
    svg_i5b_lines(svg_image_cases());
}
