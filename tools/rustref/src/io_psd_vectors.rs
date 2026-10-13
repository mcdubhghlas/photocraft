//! I2 PSD import reference vectors (`rustref io-psd-vectors`), checked by
//! tests/io/test_psd_parity.c. Lines are tab-separated; the first field names the kind.
//!
//!   fill CASE B64 RES            blocks::parse_fill of SoCo/GdFl/PtFl data (RES = Fill JSON or none)
//!   wfill JSON KEY B64           blocks::write_fill of a Fill (and parse back: fill line follows)
//!   color CASE B64 RES           blocks::color_from_desc of a descriptor (bytes = Descriptor)
//!   wcolor JSON B64              blocks::color_to_desc (descriptor bytes)
//!   lfx2 CASE B64 RES            effects_map::parse_lfx2 (RES = master + effects JSON or none)
//!   wlfx2 MASTER JSON B64        effects_map::write_lfx2
//!   lrfx CASE B64 RES            effects_map::parse_lrfx
//!   adj KEY MODE B64 CGED RES    adjust_map::parse, observed through psd_to_document
//!   wadj MODE JSON BLOCKS        adjust_map::write, observed through document_to_psd
//!   brst / adv / bif / lspf / lbl / smart / persp / warp / tysh / fxen   blocks.rs helpers
//!   tg NAME RES                  psd_to_document of psd::testgen::all_cases()[NAME]
//!   imp NAME B64 RES             psd_to_document of a synthetic file
//!   sweep NAME KIND N HASH       truncations / mutations of a synthetic file (hash of results)
//!   corp PATH RES                psd_to_document of a corpus file (dump hashed)
//!
//! RES of an import: `ok DUMP W[...]` (corpus: `ok h=HASH n=N W=HASH`), `perr "msg"` when the
//! file doesn't parse, `panic` when the reference panics. DUMP lists every document field the
//! import sets (serde JSON where the type has it, surfaces as format/bounds/default/BLAKE3), with
//! layer ids replaced by their walk index.
use std::collections::HashMap;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Arc;

use base64::Engine;
use photocraft_color::{BlendMode, Color, ColorMode, PixelFormat, SampleType};
use photocraft_doc::{AlphaChannel, Document, Effects, Fill, Layer, LayerContent, LayerMask, Size, SmartSource};
use photocraft_psd::descriptor::{Descriptor, Id, ObjectArray, UnicodeString, Value, VersionedDescriptor};
use photocraft_psd::layer::{BlendingRanges, ChannelData, LayerFlags, LayerInfo, LayerMask as PsdMask, LayerRecord, MaskData, MaskParameters, RealMask, Rect as PRect};
use photocraft_psd::resources::{ImageResource, ResolutionInfo};
use photocraft_psd::tagged::{SectionType, TaggedBlock};
use photocraft_psd::{ColorMode as PMode, Compression, PsdFile, Version};
use photocraft_raster::Surface;

#[path = "../../../crates/io/tests/common/mod.rs"]
#[allow(dead_code, unused_imports, clippy::all)]
mod io_common;

fn h16(b: &[u8]) -> String {
    blake3::hash(b).to_hex()[..16].to_string()
}
fn b64(b: &[u8]) -> String {
    base64::engine::general_purpose::STANDARD.encode(b)
}
fn hexs(b: &[u8]) -> String {
    b.iter().map(|v| format!("{v:02x}")).collect()
}
fn opt_h(o: Option<&[u8]>) -> String {
    o.map_or("-".into(), |b| format!("{}:{}", b.len(), h16(b)))
}
fn fbits(v: f32) -> String {
    format!("{:08x}", v.to_bits())
}
fn dbits(v: f64) -> String {
    format!("{:016x}", v.to_bits())
}
macro_rules! js {
    ($e:expr) => {
        serde_json::to_string($e).unwrap_or_else(|e| format!("<json error {e}>"))
    };
}
fn vd(d: Descriptor) -> Vec<u8> {
    VersionedDescriptor::new(d).to_bytes()
}
fn en(t: &str, v: &str) -> Value {
    Value::Enumerated { type_id: Id::new(t), value: Id::new(v) }
}
fn txt(s: &str) -> Value {
    Value::Text(UnicodeString::new_nul(s))
}
fn uf(u: &[u8; 4], v: f64) -> Value {
    Value::UnitFloat { unit: *u, value: v }
}

// ---------------------------------------------------------------------------------------------
// document dump (mirrored by tests/io/test_psd_parity.c)
// ---------------------------------------------------------------------------------------------

fn surf(s: &Surface) -> String {
    let fmt = s.format();
    let b = s.content_bounds();
    let mut def = vec![0u8; fmt.channels() * fmt.sample.bytes()];
    photocraft_raster::encode_pixel(&fmt, &s.default_pixel(), &mut def);
    let px = if b.is_empty() { h16(&[]) } else { h16(&s.to_interleaved(b)) };
    format!("{:?}/{:?}/{}:{},{},{},{}:{}:{}", fmt.mode, fmt.sample, u8::from(fmt.alpha), b.x0, b.y0, b.x1, b.y1, hexs(&def), px)
}

fn color_s(c: &Color) -> String {
    format!("{:?}:{}:{}:{}:{}:{}", c.mode, fbits(c.c[0]), fbits(c.c[1]), fbits(c.c[2]), fbits(c.c[3]), fbits(c.alpha))
}

fn chan_sum(c: &AlphaChannel) -> String {
    let spot = c.spot.as_ref().map_or("-".into(), |(col, s)| format!("{}@{}", color_s(col), fbits(*s)));
    let ind = if c.indicates == photocraft_doc::ColorIndicates::SelectedAreas { "sel" } else { "mask" };
    format!("{}|{}|{}|{}|{}|{}", js!(&c.name), spot, color_s(&c.color), fbits(c.opacity), ind, surf(&c.surface))
}

fn mask_s(m: &Option<LayerMask>) -> String {
    m.as_ref().map_or("-".into(), |m| format!("{}{}:{}:{}:{}", u8::from(m.enabled), u8::from(m.linked), fbits(m.density), fbits(m.feather), surf(&m.surface)))
}

fn effects_s(e: &Effects) -> String {
    let r = e.reference.map_or("-".into(), |(x, y)| format!("{},{}", dbits(x), dbits(y)));
    format!("{}:{}:{}:{}", u8::from(e.enabled), js!(&e.items), opt_h(e.psd_raw.as_deref().map(|v| v.as_slice())), r)
}

fn opt_surf(s: &Option<Surface>) -> String {
    s.as_ref().map_or("-".into(), surf)
}

fn raw_s(r: &Option<Arc<Vec<u8>>>) -> String {
    opt_h(r.as_deref().map(|v| v.as_slice()))
}

fn warp_s(w: &Option<photocraft_geom::warp::Warp>) -> String {
    let Some(w) = w else { return "-".into() };
    let mut s = format!(
        "{},{},{},{},{},{},{},{},{}",
        w.style.id(),
        dbits(w.bend),
        dbits(w.h_distort),
        dbits(w.v_distort),
        u8::from(w.vertical),
        dbits(w.bounds[0]),
        dbits(w.bounds[1]),
        dbits(w.bounds[2]),
        dbits(w.bounds[3])
    );
    if let Some(m) = &w.mesh {
        s.push_str(":m");
        for v in m.us.iter().chain(&m.vs) {
            s.push_str(&format!(",{}", dbits(*v)));
        }
        for p in &m.points {
            s.push_str(&format!(",{}/{}", dbits(p[0]), dbits(p[1])));
        }
    }
    s
}

fn content_s(c: &LayerContent, ids: &HashMap<u64, u64>) -> String {
    match c {
        LayerContent::Raster(s) => format!("R:{}", surf(s)),
        LayerContent::Group(g) => format!(
            "G:{}:{}{{{}}}",
            u8::from(g.expanded),
            g.artboard.as_ref().map_or("-".into(), |x| js!(x)),
            g.children.iter().map(|l| layer_s(l, ids)).collect::<Vec<_>>().join(";")
        ),
        LayerContent::Adjustment(a) => format!("A:{}", js!(a)),
        LayerContent::Fill(f) => format!("F:{}", js!(f)),
        LayerContent::Text(t) => format!(
            "T:{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
            js!(&t.text),
            js!(&t.font_family),
            fbits(t.size_pt),
            color_s(&t.color),
            js!(&t.transform),
            opt_surf(&t.cache),
            raw_s(&t.psd_raw),
            js!(&t.runs),
            js!(&t.paragraphs),
            js!(&t.shape),
            js!(&t.orientation),
            js!(&t.antialias),
            t.warp.as_ref().map_or("-".into(), |x| js!(x))
        ),
        LayerContent::Shape(s) => format!(
            "S:{}|{}|{}|{}|{}|{}",
            js!(&s.path),
            s.fill.as_ref().map_or("-".into(), |x| js!(x)),
            s.stroke.as_ref().map_or("-".into(), |x| js!(x)),
            s.live.as_ref().map_or("-".into(), |x| js!(x)),
            opt_surf(&s.cache),
            raw_s(&s.psd_raw)
        ),
        LayerContent::Smart(o) => {
            let src = match &o.source {
                SmartSource::Linked { path } => format!("link:{}", js!(path)),
                SmartSource::Embedded { file_name, bytes } => format!("emb:{}:{}", js!(file_name), opt_h(Some(bytes.as_slice()))),
            };
            let persp = o.perspective.map_or("-".into(), |p| p.iter().map(|v| dbits(*v)).collect::<Vec<_>>().join(","));
            format!(
                "O:{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
                src,
                js!(&o.transform),
                js!(&o.smart_filters),
                opt_surf(&o.cache),
                raw_s(&o.psd_raw),
                u8::from(o.filters_enabled),
                mask_s(&o.filter_mask),
                warp_s(&o.warp),
                o.stack_mode.map_or("-".into(), |m| m.id().to_string()),
                persp
            )
        }
    }
}

fn layer_s(l: &Layer, ids: &HashMap<u64, u64>) -> String {
    let blocks = l.psd_blocks.iter().map(|(k, d)| format!("{}:{}", hexs(k), opt_h(Some(d.as_slice())))).collect::<Vec<_>>().join(",");
    let fc = l.fill_cache.as_ref().map_or("-".into(), |f| format!("{}:{}", js!(&f.fill), surf(&f.surface)));
    let _ = ids;
    format!(
        "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|M:{}|V:{}|X:{}|B:[{}]|C:{}|{}",
        js!(&l.name),
        u8::from(l.visible),
        js!(&l.locks),
        js!(&l.blend),
        fbits(l.opacity),
        fbits(l.fill_opacity),
        u8::from(l.clipped),
        js!(&l.label),
        l.psd_id.map_or("-".into(), |v| v.to_string()),
        l.link_group.map_or("-".into(), |v| v.to_string()),
        l.excluded_channels,
        js!(&l.blend_if),
        js!(&l.advanced),
        mask_s(&l.mask),
        l.vector_mask.as_ref().map_or("-".into(), |x| js!(x)),
        effects_s(&l.effects),
        blocks,
        fc,
        content_s(&l.content, ids)
    )
}

fn comp_s(c: &photocraft_doc::LayerComp, ids: &HashMap<u64, u64>) -> String {
    let states = c
        .states
        .iter()
        .map(|s| {
            let vis = s.visible.map_or("-".into(), |v| u8::from(v).to_string());
            let pos = s.position.map_or("-".into(), |(x, y)| format!("{x},{y}"));
            let app = s.appearance.as_ref().map_or("-".into(), |a| format!("{}/{}/{}/{}", js!(&a.blend), fbits(a.opacity), fbits(a.fill_opacity), effects_s(&a.effects)));
            format!("{}:{}:{}:{}", ids.get(&s.layer.0).copied().unwrap_or(0), vis, pos, app)
        })
        .collect::<Vec<_>>()
        .join(",");
    format!(
        "{}|{}|{}|{}{}{}|[{}]",
        c.id,
        js!(&c.name),
        js!(&c.comment),
        u8::from(c.apply_visibility),
        u8::from(c.apply_position),
        u8::from(c.apply_appearance),
        states
    )
}

fn walk_ids(d: &Document) -> HashMap<u64, u64> {
    d.walk().iter().enumerate().map(|(i, (_, _, l))| (l.id.0, i as u64 + 1)).collect()
}

fn doc_dump(d: &Document) -> String {
    let ids = walk_ids(d);
    let res = d.metadata.psd_resources.iter().map(|(id, n, data)| format!("{id}:{}:{}", js!(n), opt_h(Some(data.as_slice())))).collect::<Vec<_>>().join(",");
    let gb = d.metadata.psd_global_blocks.iter().map(|(s, k, data)| format!("{}/{}:{}", hexs(s), hexs(k), opt_h(Some(data.as_slice())))).collect::<Vec<_>>().join(",");
    let paths = d.paths.iter().map(|p| format!("{}:{}:{}", js!(&p.name), js!(&p.path), raw_s(&p.psd_raw))).collect::<Vec<_>>().join(",");
    let pats = d.patterns.iter().map(|p| format!("{}|{}|{}x{}|{}", js!(&p.id), js!(&p.name), p.width, p.height, surf(&p.surface))).collect::<Vec<_>>().join(";");
    let mut slices = d.slices.clone();
    for s in &mut slices.list {
        s.layer = s.layer.map(|l| photocraft_doc::LayerId(ids.get(&l.0).copied().unwrap_or(0)));
    }
    format!(
        "{}x{} {} {} dpi={} icc={} exif={} xmp={} guides={} light={} res=[{}] gb=[{}] paths=[{}] work={} clip={} pat=[{}] ct={} duo={} comps=[{}] la={} ls={} notes={} meas={} ts={} sl={} L[{}] C[{}] Q={}",
        d.size.width,
        d.size.height,
        js!(&d.mode),
        js!(&d.depth),
        fbits(d.resolution_dpi),
        opt_h(d.icc_profile.as_deref().map(|v| v.as_slice())),
        opt_h(d.metadata.exif.as_deref().map(|v| v.as_slice())),
        opt_h(d.metadata.xmp.as_deref().map(str::as_bytes)),
        js!(&d.guides),
        js!(&d.global_light),
        res,
        gb,
        paths,
        d.work_path.as_ref().map_or("-".into(), |x| js!(x)),
        d.clipping_path.as_ref().map_or("-".into(), |x| js!(x)),
        pats,
        d.color_table.as_ref().map_or("-".into(), |x| js!(x)),
        d.duotone.as_ref().map_or("-".into(), |x| js!(x)),
        d.layer_comps.iter().map(|c| comp_s(c, &ids)).collect::<Vec<_>>().join(";"),
        d.last_applied_comp.map_or("-".into(), |v| v.to_string()),
        d.last_document_state.as_ref().map_or("-".into(), |c| comp_s(c, &ids)),
        js!(&d.notes),
        js!(&d.measurement),
        js!(&d.text_styles),
        js!(&slices),
        d.layers.iter().map(|l| layer_s(l, &ids)).collect::<Vec<_>>().join(";"),
        d.channels.iter().map(chan_sum).collect::<Vec<_>>().join(";"),
        d.quick_mask.as_ref().map_or("-".into(), chan_sum)
    )
}

/// Warnings cut at an embedded U+0000, as C stores them (REWRITE_PLAN.md §7, I1).
fn warn_s(w: &[String]) -> String {
    let cut: Vec<&str> = w.iter().map(|s| s.split('\0').next().unwrap_or("")).collect();
    format!("W{}", js!(&cut))
}

/// psd_to_document of bytes: (full result, hashed result).
pub fn import(bytes: &[u8]) -> (String, String) {
    let f = match PsdFile::from_bytes(bytes) {
        Ok(f) => f,
        Err(e) => {
            let s = format!("perr {}", js!(&e.to_string()));
            return (s.clone(), s);
        }
    };
    match catch_unwind(AssertUnwindSafe(|| photocraft_io::psd_to_document(&f))) {
        Ok((d, w)) => {
            let dump = doc_dump(&d);
            let ws = warn_s(&w);
            (format!("ok {dump} {ws}"), format!("ok h={} n={} W={}", h16(dump.as_bytes()), w.len(), h16(ws.as_bytes())))
        }
        Err(_) => ("panic".into(), "panic".into()),
    }
}

// ---------------------------------------------------------------------------------------------
// blocks.rs helpers
// ---------------------------------------------------------------------------------------------

fn sample_gradient(n: usize) -> Descriptor {
    let mut clrs = Vec::new();
    for i in 0..n {
        let c = Descriptor::new("RGBC").with("Rd  ", Value::Double(255.0 * i as f64 / n as f64)).with("Grn ", Value::Double(30.0)).with("Bl  ", Value::Integer(200));
        clrs.push(Value::Descriptor(
            Descriptor::new("Clrt")
                .with("Clr ", Value::Descriptor(c))
                .with("Type", en("Clry", "UsrS"))
                .with("Lctn", Value::Integer(((n - i) * 4096 / n) as i32))
                .with("Mdpn", Value::Integer(30 + 10 * i as i32)),
        ));
    }
    let trns = vec![
        Value::Descriptor(Descriptor::new("TrnS").with("Opct", uf(b"#Prc", 100.0)).with("Lctn", Value::Integer(0)).with("Mdpn", Value::Integer(50))),
        Value::Descriptor(Descriptor::new("TrnS").with("Opct", uf(b"#Prc", 25.0)).with("Lctn", Value::Integer(4096)).with("Mdpn", Value::Integer(70))),
    ];
    Descriptor::new("Grdn").with("Nm  ", txt("G")).with("Intr", Value::Double(0.0)).with("Clrs", Value::List(clrs)).with("Trns", Value::List(trns))
}

fn fill_cases() -> Vec<(String, [u8; 4], Vec<u8>)> {
    let mut v = Vec::new();
    let soco = |c: Descriptor| vd(Descriptor::new("null").with("Clr ", Value::Descriptor(c)));
    v.push(("soco-rgb".into(), *b"SoCo", soco(Descriptor::new("RGBC").with("Rd  ", Value::Double(255.0)).with("Grn ", Value::Double(128.0)).with("Bl  ", Value::Double(0.0)))));
    v.push(("soco-float".into(), *b"SoCo", soco(Descriptor::new("RGBC").with("redFloat", Value::Double(0.25)).with("greenFloat", Value::Double(0.5)).with("blueFloat", Value::Double(1.5)))));
    v.push(("soco-cmyk".into(), *b"SoCo", soco(Descriptor::new("CMYC").with("Cyn ", Value::Double(10.0)).with("Mgnt", uf(b"#Prc", 20.0)).with("Ylw ", Value::Integer(30)).with("Blck", Value::Double(40.0)))));
    v.push(("soco-gray".into(), *b"SoCo", soco(Descriptor::new("Grsc").with("Gry ", Value::Double(75.0)))));
    v.push(("soco-lab".into(), *b"SoCo", soco(Descriptor::new("LbCl").with("Lmnc", Value::Double(19.07)).with("A   ", Value::Double(52.29)).with("B   ", Value::Double(-85.08)))));
    v.push(("soco-hsb".into(), *b"SoCo", soco(Descriptor::new("HSBC").with("H   ", Value::Double(10.0)))));
    v.push(("soco-missing".into(), *b"SoCo", vd(Descriptor::new("null"))));
    let mut pad = soco(Descriptor::new("RGBC").with("Rd  ", Value::Double(1.0)).with("Grn ", Value::Double(2.0)).with("Bl  ", Value::Double(3.0)));
    pad.extend_from_slice(&[0, 0, 0]);
    v.push(("soco-padded".into(), *b"SoCo", pad));
    v.push(("garbage".into(), *b"SoCo", vec![0, 0, 0, 16, 1, 2]));
    v.push(("empty".into(), *b"GdFl", Vec::new()));
    let gd = |extra: Vec<(&str, Value)>, n: usize| {
        let mut d = Descriptor::new("null").with("Grad", Value::Descriptor(sample_gradient(n)));
        for (k, val) in extra {
            d = d.with(k, val);
        }
        vd(d)
    };
    v.push(("gdfl-plain".into(), *b"GdFl", gd(vec![], 3)));
    v.push((
        "gdfl-full".into(),
        *b"GdFl",
        gd(
            vec![
                ("Angl", uf(b"#Ang", -33.5)),
                ("Type", en("GrdT", "Rflc")),
                ("Scl ", uf(b"#Prc", 225.0)),
                ("Rvrs", Value::Boolean(true)),
                ("Dthr", Value::Boolean(true)),
                ("Algn", Value::Boolean(false)),
                ("Ofst", Value::Descriptor(Descriptor::new("Pnt ").with("Hrzn", uf(b"#Prc", 12.5)).with("Vrtc", uf(b"#Prc", -37.5)))),
            ],
            2,
        ),
    ));
    for (name, m) in [("perc", "Perc"), ("lnr", "Lnr "), ("gcls", "Gcls")] {
        v.push((format!("gdfl-{name}"), *b"GdFl", gd(vec![("gs99", en("gradientInterpolationMethodType", m))], 4)));
    }
    v.push(("gdfl-nostops".into(), *b"GdFl", vd(Descriptor::new("null").with("Angl", Value::Integer(45)))));
    let mut smooth = sample_gradient(3);
    smooth.items.retain(|(k, _)| !k.is("Intr"));
    let smooth = smooth.with("Intr", Value::Double(4096.0));
    v.push(("gdfl-smooth".into(), *b"GdFl", vd(Descriptor::new("null").with("Grad", Value::Descriptor(smooth)))));
    let lab = Descriptor::new("Grdn").with(
        "Clrs",
        Value::List(
            [0, 4096]
                .iter()
                .map(|l| {
                    Value::Descriptor(
                        Descriptor::new("Clrt")
                            .with("Clr ", Value::Descriptor(Descriptor::new("LbCl").with("Lmnc", Value::Double(50.0)).with("A   ", Value::Double(*l as f64 / 100.0)).with("B   ", Value::Double(-3.0))))
                            .with("Lctn", Value::Integer(*l)),
                    )
                })
                .collect(),
        ),
    );
    v.push(("gdfl-lab".into(), *b"GdFl", vd(Descriptor::new("null").with("Grad", Value::Descriptor(lab)))));
    let ptfl = Descriptor::new("null")
        .with("Scl ", uf(b"#Prc", 50.0))
        .with("Ptrn", Value::Descriptor(Descriptor::new("Ptrn").with("Nm  ", txt("Bubbles")).with("Idnt", txt("abc-123"))))
        .with("Angl", uf(b"#Ang", 30.0))
        .with("Algn", Value::Boolean(false))
        .with("phase", Value::Descriptor(Descriptor::new("Pnt ").with("Hrzn", Value::Double(3.0)).with("Vrtc", Value::Double(-2.0))));
    v.push(("ptfl".into(), *b"PtFl", vd(ptfl)));
    v.push(("ptfl-bare".into(), *b"PtFl", vd(Descriptor::new("null"))));
    v.push(("wrongkey".into(), *b"XXXX", vd(Descriptor::new("null"))));
    v
}

fn fills() {
    for (name, k, data) in fill_cases() {
        let r = photocraft_io::blocks::parse_fill(&k, &data).map_or("none".into(), |f| js!(&f));
        println!("fill\t{name}\t{}\t{}\t{r}", hexs(&k), b64(&data));
    }
    let red = Color::rgba(1.0, 0.0, 0.0, 1.0);
    let fs = vec![
        Fill::Solid(Color::rgb(1.0, 0.5, 0.0)),
        Fill::Solid(Color { mode: ColorMode::Cmyk, c: [0.1, 0.2, 0.3, 0.4], alpha: 1.0 }),
        Fill::Solid(Color::gray(0.25)),
        Fill::Solid(Color { mode: ColorMode::Lab, c: [0.5, 0.25, 0.75, 0.0], alpha: 1.0 }),
        Fill::gradient(vec![(0.0, Color::rgb(1.0, 0.0, 0.0)), (1.0, Color::rgb(0.0, 0.0, 1.0))], 45.0, 1.0, photocraft_doc::GradientStyle::Reflected, true),
        Fill::gradient(vec![(0.0, red), (1.0, Color { alpha: 0.0, ..red })], 90.0, 1.0, photocraft_doc::GradientStyle::Linear, false),
        Fill::Gradient {
            stops: vec![(0.0, Color::rgb(1.0, 0.0, 0.0)), (0.5, Color::rgb(0.0, 1.0, 0.0)), (1.0, Color::rgb(0.0, 0.0, 1.0))],
            angle: -33.5,
            scale: 2.25,
            style: photocraft_doc::GradientStyle::Radial,
            reverse: false,
            opacity_stops: vec![(0.0, 1.0), (0.25, 0.5), (1.0, 0.0)],
            midpoints: vec![0.25, 0.75],
            offset: (0.125, -0.375),
            dither: true,
            align: false,
        },
        Fill::Gradient {
            stops: vec![(0.7, Color::rgb(1.0, 1.0, 0.0)), (0.1, Color::rgb(0.0, 1.0, 1.0)), (0.4, Color::gray(0.5))],
            angle: 0.0,
            scale: 1.0,
            style: photocraft_doc::GradientStyle::Diamond,
            reverse: false,
            opacity_stops: vec![],
            midpoints: vec![0.01, 0.99],
            offset: (0.0, 0.5),
            dither: false,
            align: true,
        },
        Fill::Pattern { name: "Bubbles".into(), scale: 0.5, id: "abc".into(), angle: 30.0, link: false, phase: (3.0, -2.0) },
        Fill::Pattern { name: "N\u{e9}".into(), scale: 1.0, id: String::new(), angle: 0.0, link: true, phase: (0.0, 0.0) },
    ];
    for f in fs {
        let (k, data) = photocraft_io::blocks::write_fill(&f);
        let back = photocraft_io::blocks::parse_fill(&k, &data).map_or("none".into(), |f| js!(&f));
        println!("wfill\t{}\t{}\t{}\t{back}", js!(&f), hexs(&k), b64(&data));
    }
}

fn color_cases() -> Vec<(String, Descriptor)> {
    vec![
        ("rgb".into(), Descriptor::new("RGBC").with("Rd  ", Value::Double(12.0)).with("Grn ", Value::Integer(250)).with("Bl  ", uf(b"#Prc", 3.5))),
        ("rgb-missing".into(), Descriptor::new("RGBC").with("Rd  ", Value::Double(12.0))),
        ("rgbfloat".into(), Descriptor::new("RGBC").with("redFloat", Value::Double(1.0)).with("greenFloat", Value::Double(0.5)).with("blueFloat", Value::Double(0.25))),
        ("rgbfloat-partial".into(), Descriptor::new("RGBC").with("redFloat", Value::Double(1.0)).with("Grn ", Value::Double(1.0))),
        ("cmyk".into(), Descriptor::new("CMYC").with("Cyn ", Value::Double(1.0)).with("Mgnt", Value::Double(2.0)).with("Ylw ", Value::Double(3.0)).with("Blck", Value::Double(4.0))),
        ("cmyk-bad".into(), Descriptor::new("CMYC").with("Cyn ", Value::Boolean(true))),
        ("gray".into(), Descriptor::new("Grsc").with("Gry ", Value::Double(33.3))),
        ("lab".into(), Descriptor::new("LbCl").with("Lmnc", Value::Double(19.07)).with("A   ", Value::Double(52.29)).with("B   ", Value::Double(-85.08))),
        ("lab-missing".into(), Descriptor::new("LbCl").with("Lmnc", Value::Double(50.0))),
        ("hsb".into(), Descriptor::new("HSBC").with("H   ", uf(b"#Ang", 10.0)).with("Strt", Value::Double(1.0)).with("Brgh", Value::Double(1.0))),
        ("other-rgb".into(), Descriptor::new("XXXX").with("Rd  ", Value::Double(1.0)).with("Grn ", Value::Double(2.0)).with("Bl  ", Value::Double(3.0))),
        ("other-float".into(), Descriptor::new("bookColor").with("redFloat", Value::Double(0.1)).with("greenFloat", Value::Double(0.2)).with("blueFloat", Value::Double(0.3))),
        ("huge".into(), Descriptor::new("RGBC").with("Rd  ", Value::Double(1e300)).with("Grn ", Value::Double(f64::NAN)).with("Bl  ", Value::Double(-1e-300))),
    ]
}

fn colors() {
    for (name, d) in color_cases() {
        let r = photocraft_io::blocks::color_from_desc(&d).map_or("none".into(), |c| color_s(&c));
        println!("color\t{name}\t{}\t{r}", b64(&d.to_bytes()));
    }
    for c in [
        Color::rgb(0.1, 0.2, 0.3),
        Color::rgba(1.0, 0.5, 0.25, 0.5),
        Color::gray(0.4),
        Color { mode: ColorMode::Cmyk, c: [0.1, 0.2, 0.3, 0.4], alpha: 1.0 },
        Color { mode: ColorMode::Lab, c: [0.5, 0.2, 0.9, 0.0], alpha: 1.0 },
        Color { mode: ColorMode::Indexed, c: [0.5, 0.2, 0.9, 0.0], alpha: 1.0 },
        Color { mode: ColorMode::Duotone, c: [0.7, 0.0, 0.0, 0.0], alpha: 1.0 },
    ] {
        let d = photocraft_io::blocks::color_to_desc(&c);
        println!("wcolor\t{}\t{}", js!(&c), b64(&d.to_bytes()));
    }
}

// ---------------------------------------------------------------------------------------------
// effects_map.rs
// ---------------------------------------------------------------------------------------------

fn sample_effects() -> Vec<photocraft_doc::Effect> {
    use photocraft_doc::adjust::CurvePoint;
    use photocraft_doc::*;
    let g = Gradient { stops: vec![(0.0, Color::rgb(1.0, 0.0, 0.0)), (1.0, Color::rgb(0.0, 0.0, 1.0))], opacity_stops: vec![(0.0, 1.0), (1.0, 0.5)], ..Gradient::default() };
    let ds = match Effect::default_drop_shadow() {
        Effect::DropShadow(s) => s,
        _ => return Vec::new(),
    };
    vec![
        Effect::default_drop_shadow(),
        Effect::DropShadow(Shadow {
            distance: 9.0,
            spread: 0.25,
            contour: Contour::Custom {
                name: "Cone".into(),
                points: vec![CurvePoint { input: 0.0, output: 0.0 }, CurvePoint { input: 0.5, output: 1.0 }, CurvePoint { input: 1.0, output: 0.0 }],
                corners: Vec::new(),
            },
            ..ds.clone()
        }),
        Effect::InnerShadow(Shadow { knocks_out: false, color: Color { mode: ColorMode::Cmyk, c: [0.1, 0.2, 0.3, 0.4], alpha: 1.0 }, ..ds }),
        Effect::OuterGlow(Glow {
            common: FxCommon::new(BlendMode::Screen, 0.5),
            paint: FxPaint::Color(Color::rgb(1.0, 1.0, 0.0)),
            technique: GlowTechnique::Precise,
            spread: 0.1,
            size: 8.0,
            contour: Contour::Linear,
            anti_alias: true,
            range: 0.5,
            jitter: 0.0,
            noise: 0.0,
            source: GlowSource::Edge,
        }),
        Effect::InnerGlow(Glow {
            common: FxCommon::new(BlendMode::LinearDodge, 0.75),
            paint: FxPaint::Gradient(g.clone()),
            technique: GlowTechnique::Softer,
            spread: 0.0,
            size: 5.0,
            contour: Contour::Linear,
            anti_alias: false,
            range: 0.3,
            jitter: 0.2,
            noise: 0.1,
            source: GlowSource::Center,
        }),
        Effect::OuterGlow(Glow {
            common: FxCommon::new(BlendMode::Screen, 0.5),
            paint: FxPaint::Pattern { name: "P".into(), id: "x".into(), scale: 1.0 },
            technique: GlowTechnique::Softer,
            spread: 0.0,
            size: 1.0,
            contour: Contour::Linear,
            anti_alias: false,
            range: 0.5,
            jitter: 0.0,
            noise: 0.0,
            source: GlowSource::Edge,
        }),
        Effect::Stroke(StrokeFx { common: FxCommon::new(BlendMode::Normal, 1.0), size: 3.0, position: StrokePosition::Inside, paint: FxPaint::Color(Color::rgb(0.0, 1.0, 0.0)) }),
        Effect::Stroke(StrokeFx { common: FxCommon::new(BlendMode::Multiply, 0.5), size: 6.0, position: StrokePosition::Center, paint: FxPaint::Gradient(g.clone()) }),
        Effect::Stroke(StrokeFx {
            common: FxCommon::new(BlendMode::Darken, 0.25),
            size: 2.0,
            position: StrokePosition::Outside,
            paint: FxPaint::Pattern { name: "Bubbles".into(), id: "abc".into(), scale: 0.5 },
        }),
        Effect::ColorOverlay { common: FxCommon::new(BlendMode::Overlay, 0.5), color: Color::rgb(0.0, 0.0, 1.0) },
        Effect::ColorOverlay { common: FxCommon { enabled: false, blend: BlendMode::PassThrough, opacity: 1.0 }, color: Color::gray(0.5) },
        Effect::GradientOverlay { common: FxCommon::new(BlendMode::Normal, 1.0), gradient: g, dither: true },
        Effect::PatternOverlay { common: FxCommon::new(BlendMode::Normal, 1.0), name: "Bubbles".into(), id: "abc".into(), scale: 1.0, angle: 15.0, link: false, phase: (4.0, 2.0) },
        Effect::Satin(Satin { common: FxCommon::new(BlendMode::Multiply, 0.5), color: Color::BLACK, angle: 19.0, distance: 11.0, size: 14.0, contour: Contour::Linear, anti_alias: true, invert: true }),
        Effect::BevelEmboss(Bevel {
            enabled: true,
            style: BevelStyle::Emboss,
            technique: BevelTechnique::ChiselSoft,
            depth: 1.5,
            up: false,
            size: 7.0,
            soften: 2.0,
            angle: 45.0,
            altitude: 40.0,
            use_global_light: false,
            gloss_contour: Contour::Linear,
            highlight: FxCommon::new(BlendMode::Screen, 0.75),
            highlight_color: Color::WHITE,
            shadow: FxCommon::new(BlendMode::Multiply, 0.6),
            shadow_color: Color::BLACK,
            contour: Some(BevelContour { contour: Contour::Linear, range: 0.7, anti_alias: true }),
            texture: Some(BevelTexture { name: "Bubbles".into(), id: "abc".into(), scale: 0.5, depth: -2.0, invert: true, link: false, phase: (3.0, 1.0) }),
        }),
        Effect::BevelEmboss(Bevel {
            enabled: false,
            style: BevelStyle::StrokeEmboss,
            technique: BevelTechnique::ChiselHard,
            depth: 1.0,
            up: true,
            size: 5.0,
            soften: 0.0,
            angle: 120.0,
            altitude: 30.0,
            use_global_light: true,
            gloss_contour: Contour::Custom { name: String::new(), points: vec![CurvePoint { input: 0.0, output: 1.0 }, CurvePoint { input: 1.0, output: 0.0 }], corners: Vec::new() },
            highlight: FxCommon::new(BlendMode::ColorDodge, 0.5),
            highlight_color: Color::gray(0.9),
            shadow: FxCommon::new(BlendMode::ColorBurn, 0.6),
            shadow_color: Color::rgb(0.2, 0.0, 0.0),
            contour: None,
            texture: None,
        }),
    ]
}

fn lfx2_desc(d: Descriptor) -> Vec<u8> {
    let mut v = 0u32.to_be_bytes().to_vec();
    v.extend(vd(d));
    v
}

fn lfx2_cases() -> Vec<(String, Vec<u8>)> {
    let long = |m: &str| en("BlnM", m);
    let mut v = vec![
        ("garbage".to_string(), vec![0, 0]),
        ("empty".into(), Vec::new()),
        ("bad-version".into(), {
            let mut d = lfx2_desc(Descriptor::new("null"));
            d[7] = 17;
            d
        }),
        (
            "absent-present".into(),
            lfx2_desc(
                Descriptor::new("null")
                    .with("DrSh", Value::Descriptor(Descriptor::new("DrSh").with("present", Value::Boolean(false)).with("enab", Value::Boolean(false))))
                    .with("FrFX", Value::Descriptor(Descriptor::new("FrFX").with("present", Value::Boolean(true)).with("enab", Value::Boolean(false))))
                    .with("SoFi", Value::Descriptor(Descriptor::new("SoFi").with("enab", Value::Boolean(false)))),
            ),
        ),
        (
            "long-ids".into(),
            lfx2_desc(
                Descriptor::new("null")
                    .with("masterFXSwitch", Value::Boolean(true))
                    .with("DrSh", Value::Descriptor(Descriptor::new("DrSh").with("enab", Value::Boolean(true)).with("Md  ", long("linearBurn"))))
                    .with("ebbl", Value::Descriptor(Descriptor::new("ebbl").with("hglM", long("colorDodge")).with("sdwM", long("colorBurn"))))
                    .with("SoFi", Value::Descriptor(Descriptor::new("SoFi").with("Md  ", long("hue")))),
            ),
        ),
        (
            "multi-global".into(),
            lfx2_desc(
                Descriptor::new("null")
                    .with("masterFXSwitch", Value::Boolean(false))
                    .with("dropShadowMulti", Value::List(vec![Value::Descriptor(Descriptor::new("DrSh")), Value::Integer(3), Value::GlobalObject(Descriptor::new("DrSh"))]))
                    .with("DrSh", Value::Descriptor(Descriptor::new("DrSh").with("Dstn", Value::Double(99.0))))
                    .with("IrSh", Value::GlobalObject(Descriptor::new("IrSh").with("blur", Value::Integer(7))))
                    .with("innerShadowMulti", Value::Integer(1))
                    .with("OrGl", Value::Descriptor(Descriptor::new("OrGl").with("Grad", Value::Descriptor(sample_gradient(2))).with("gs99", en("x", "Perc"))))
                    .with("unknownFX", Value::Descriptor(Descriptor::new("x"))),
            ),
        ),
    ];
    for (i, m) in ["Nrml", "Dslv", "linearBurn", "darkerColor", "H   ", "passThrough", "multiply", "luminosity", "bogus", "Lmns", "blendDivide"].iter().enumerate() {
        v.push((
            format!("blend{i}"),
            lfx2_desc(Descriptor::new("null").with("SoFi", Value::Descriptor(Descriptor::new("SoFi").with("Md  ", en("BlnM", m)).with("Opct", Value::Integer(i as i32 * 9))))),
        ));
    }
    v
}

fn lrfx_record(key: &[u8; 4], body: &[u8]) -> Vec<u8> {
    let mut v = b"8BIM".to_vec();
    v.extend_from_slice(key);
    v.extend_from_slice(&(body.len() as u32).to_be_bytes());
    v.extend_from_slice(body);
    v
}

fn lrfx_cases() -> Vec<(String, Vec<u8>)> {
    let shadow = |blend: &[u8; 4]| {
        let mut r = Vec::new();
        r.extend_from_slice(&2u32.to_be_bytes());
        r.extend_from_slice(&(7u32 << 16).to_be_bytes());
        r.extend_from_slice(&0u32.to_be_bytes());
        r.extend_from_slice(&((-90i32 as u32) << 16).to_be_bytes());
        r.extend_from_slice(&((4u32 << 16) | 0x8000).to_be_bytes());
        r.extend_from_slice(&[0, 0, 0xff, 0xff, 0x80, 0x10, 0x12, 0x34, 0, 0]);
        r.extend_from_slice(b"8BIM");
        r.extend_from_slice(blend);
        r.extend_from_slice(&[1, 0, 128]);
        r.extend_from_slice(&[0; 10]);
        r
    };
    let glow = {
        let mut r = Vec::new();
        r.extend_from_slice(&2u32.to_be_bytes());
        r.extend_from_slice(&(3u32 << 16).to_be_bytes());
        r.extend_from_slice(&0u32.to_be_bytes());
        r.extend_from_slice(&[0, 0, 0x10, 0, 0x20, 0, 0x30, 0, 0, 0]);
        r.extend_from_slice(b"8BIMscrn");
        r.extend_from_slice(&[1, 200]);
        r.extend_from_slice(&[0; 6]);
        r
    };
    let sofi = {
        let mut r = Vec::new();
        r.extend_from_slice(&2u32.to_be_bytes());
        r.extend_from_slice(b"8BIMdiv ");
        r.extend_from_slice(&[0, 0, 0xaa, 0xaa, 0xbb, 0xbb, 0xcc, 0xcc, 0, 0]);
        r.extend_from_slice(&[77, 1]);
        r.extend_from_slice(&[0; 4]);
        r
    };
    let mut all = vec![0, 0, 0, 6];
    all.extend(lrfx_record(b"cmnS", &[0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0]));
    all.extend(lrfx_record(b"dsdw", &shadow(b"mul ")));
    all.extend(lrfx_record(b"isdw", &shadow(b"zzzz")));
    all.extend(lrfx_record(b"oglw", &glow));
    all.extend(lrfx_record(b"iglw", &glow));
    all.extend(lrfx_record(b"sofi", &sofi));
    let mut v = vec![("all".to_string(), all.clone()), ("garbage".into(), vec![0, 0, 0, 1, b'X']), ("none".into(), vec![0, 0, 0, 0]), ("short".into(), vec![0])];
    for cut in [10, 30, 60, 100, 150, all.len() - 1] {
        v.push((format!("cut{cut}"), all[..cut].to_vec()));
    }
    let mut badsig = all.clone();
    badsig[4] = b'X';
    v.push(("badsig".into(), badsig));
    let mut big = vec![0, 0, 0, 2];
    let mut rec = lrfx_record(b"dsdw", &shadow(b"norm"));
    rec[8..12].copy_from_slice(&u32::MAX.to_be_bytes());
    big.extend(rec);
    v.push(("hugesize".into(), big));
    v
}

fn effects() {
    for (name, data) in lfx2_cases() {
        let r = photocraft_io::effects_map::parse_lfx2(&data).map_or("none".into(), |(m, fx)| format!("{} {}", u8::from(m), js!(&fx)));
        println!("lfx2\t{name}\t{}\t{r}", b64(&data));
    }
    let fx = sample_effects();
    for (master, items) in [(true, fx.clone()), (false, fx[..1].to_vec()), (true, Vec::new()), (true, fx[3..9].to_vec())] {
        let data = photocraft_io::effects_map::write_lfx2(master, &items);
        let back = photocraft_io::effects_map::parse_lfx2(&data).map_or("none".into(), |(m, fx)| format!("{} {}", u8::from(m), js!(&fx)));
        println!("wlfx2\t{}\t{}\t{}\t{back}", u8::from(master), js!(&items), b64(&data));
    }
    for (name, data) in lrfx_cases() {
        let r = photocraft_io::effects_map::parse_lrfx(&data).map_or("none".into(), |(m, fx)| format!("{} {}", u8::from(m), js!(&fx)));
        println!("lrfx\t{name}\t{}\t{r}", b64(&data));
    }
}

// ---------------------------------------------------------------------------------------------
// files
// ---------------------------------------------------------------------------------------------

fn pmode(m: ColorMode) -> PMode {
    match m {
        ColorMode::Grayscale => PMode::Grayscale,
        ColorMode::Cmyk => PMode::Cmyk,
        ColorMode::Lab => PMode::Lab,
        ColorMode::Bitmap => PMode::Bitmap,
        ColorMode::Indexed => PMode::Indexed,
        ColorMode::Duotone => PMode::Duotone,
        ColorMode::Multichannel => PMode::Multichannel,
        _ => PMode::Rgb,
    }
}

/// A merged-only file of mode / depth with these layer records (no layers when empty).
fn file_with(mode: PMode, depth: u16, w: u32, h: u32, recs: Vec<LayerRecord>) -> PsdFile {
    let mut f = photocraft_psd::testgen::merged_only(Version::Psd, mode, depth, Compression::Raw, w, h);
    if !recs.is_empty() {
        f.layer_info = Some(LayerInfo { merged_alpha: false, layers: recs, padding: None });
    }
    f
}

fn bytes_of(f: &PsdFile) -> Vec<u8> {
    f.to_bytes().unwrap_or_default()
}

/// A layer record named `name` (legacy name + luni) with channels from pattern planes.
fn rec(name: &str, rect: PRect, mode: PMode, depth: u16, chans: &[i16], seed: u32) -> LayerRecord {
    let mut r = LayerRecord { rect, ..LayerRecord::default() };
    r.name = name.bytes().filter(|b| b.is_ascii()).take(255).collect();
    r.blocks.push(TaggedBlock::unicode_name(name));
    let (w, h) = rect.size().unwrap_or((0, 0));
    let _ = mode;
    for &id in chans {
        let plane = photocraft_psd::testgen::pattern_plane(w, h, depth, seed + (id + 3) as u32);
        if let Ok(c) = ChannelData::encode(id, Compression::Raw, &plane, w, h, depth, Version::Psd) {
            r.channels.push(c);
        }
    }
    r
}

fn rgb_rec(name: &str, rect: PRect, seed: u32) -> LayerRecord {
    rec(name, rect, PMode::Rgb, 8, &[-1, 0, 1, 2], seed)
}

fn blk(r: &mut LayerRecord, k: &[u8; 4], d: Vec<u8>) {
    r.blocks.push(TaggedBlock::new(*k, d));
}

fn empty_rec(name: &str) -> LayerRecord {
    rec(name, PRect::default(), PMode::Rgb, 8, &[], 0)
}

fn group_recs(name: &str, children: Vec<LayerRecord>, kind: SectionType, blend: Option<photocraft_psd::BlendMode>) -> Vec<LayerRecord> {
    let mut v = Vec::new();
    let mut div = empty_rec("</Layer group>");
    div.blocks.push(TaggedBlock::section_divider(SectionType::BoundingDivider, None, None));
    v.push(div);
    v.extend(children);
    let mut g = empty_rec(name);
    g.blocks.push(TaggedBlock::section_divider(kind, blend, None));
    v.push(g);
    v
}

fn tysh_for(text: &str) -> Vec<u8> {
    let t = photocraft_doc::TextLayer {
        text: text.into(),
        font_family: "Arial".into(),
        size_pt: 24.0,
        color: Color::rgb(1.0, 0.0, 0.0),
        transform: photocraft_geom::Affine { m: [1.0, 0.0, 0.0, 1.0, 5.0, 9.0] },
        ..Default::default()
    };
    photocraft_text::psd::build_tysh(&t, 72.0, None)
}

fn sold(desc: Descriptor) -> Vec<u8> {
    let mut v = b"soLD".to_vec();
    v.extend_from_slice(&4u32.to_be_bytes());
    v.extend(vd(desc));
    v
}

fn placed_desc(corners: [f64; 8], nonaffine: Option<[f64; 8]>, warp: Option<Descriptor>) -> Descriptor {
    let mut d = Descriptor::new("null")
        .with("Idnt", txt("uuid-1"))
        .with("placed", txt("placed-1"))
        .with("Trnf", Value::List(corners.iter().map(|v| Value::Double(*v)).collect()))
        .with("Sz  ", Value::Descriptor(Descriptor::new("Pnt ").with("Wdth", Value::Double(50.0)).with("Hght", Value::Double(25.0))));
    if let Some(q) = nonaffine {
        d = d.with("nonAffineTransform", Value::List(q.iter().map(|v| Value::Double(*v)).collect()));
    }
    if let Some(w) = warp {
        d = d.with("warp", Value::Descriptor(w));
    }
    d
}

fn warp_desc(style: &str, mesh: Option<(Vec<f64>, Vec<f64>)>) -> Descriptor {
    let bounds = Descriptor::new("Rctn").with("Top ", uf(b"#Pxl", 0.0)).with("Left", uf(b"#Pxl", 0.0)).with("Btom", uf(b"#Pxl", 30.0)).with("Rght", uf(b"#Pxl", 60.0));
    let mut w = Descriptor::new("warp")
        .with("warpStyle", en("warpStyle", style))
        .with("warpValue", Value::Double(40.0))
        .with("warpPerspective", Value::Double(5.0))
        .with("warpPerspectiveOther", Value::Double(-3.0))
        .with("warpRotate", en("Ornt", "Vrtc"))
        .with("bounds", Value::Descriptor(bounds));
    if let Some((xs, ys)) = mesh {
        let m = Descriptor::new("null").with("Hrzn", Value::UnitFloats { unit: *b"#Pxl", values: xs }).with("Vrtc", Value::UnitFloats { unit: *b"#Pxl", values: ys });
        w = w.with("customEnvelopeWarp", Value::Descriptor(Descriptor::new("customEnvelopeWarp").with("meshPoints", Value::ObjectArray(ObjectArray { prefix: 16, body: m }))));
    }
    w
}

fn mesh16(bump: f64) -> (Vec<f64>, Vec<f64>) {
    let (mut xs, mut ys) = (Vec::new(), Vec::new());
    for j in 0..4 {
        for i in 0..4 {
            xs.push(f64::from(i) * 20.0 + if i == 3 && j == 3 { bump } else { 0.0 });
            ys.push(f64::from(j) * 10.0);
        }
    }
    (xs, ys)
}

fn blocks_misc() {
    use photocraft_io::blocks::*;
    for d in [vec![], vec![0, 0, 0, 2], vec![0, 0, 0, 0, 0, 0, 0, 1], vec![0, 0, 0, 31, 0, 0, 0, 32, 255, 255, 255, 255, 0, 0, 0, 5, 9], vec![1, 2, 3]] {
        println!("brst\tp\t{}\t{}", b64(&d), parse_brst(&d));
    }
    for m in [0u32, 1, 0b101, 0x8000_0001, u32::MAX] {
        println!("brst\tw\t{m}\t{}", brst_data(m).map_or("-".into(), |d| b64(&d)));
    }
    let advs: Vec<photocraft_doc::AdvancedBlending> = vec![
        photocraft_doc::AdvancedBlending::default(),
        photocraft_doc::AdvancedBlending {
            knockout: photocraft_doc::Knockout::Deep,
            blend_interior: true,
            blend_clipped: false,
            transparency_shapes: false,
            layer_mask_hides_effects: true,
            vector_mask_hides_effects: true,
        },
        photocraft_doc::AdvancedBlending { knockout: photocraft_doc::Knockout::Shallow, ..Default::default() },
    ];
    let starts: Vec<Vec<([u8; 4], Vec<u8>)>> = vec![vec![], vec![(*b"luni", vec![0; 4]), (*b"knko", vec![1, 0, 0, 0]), (*b"clbl", vec![1]), (*b"tsly", vec![])], vec![(*b"infx", vec![1, 2, 3, 4, 5])]];
    for a in &advs {
        for (si, s) in starts.iter().enumerate() {
            let mut raw = s.clone();
            put_advanced(a, &mut raw);
            let out = raw.iter().map(|(k, d)| format!("{}:{}", hexs(k), hexs(d))).collect::<Vec<_>>().join(",");
            println!("adv\tput\t{}\t{si}\t{out}", js!(a));
        }
    }
    let flag_sets: Vec<Vec<(&[u8; 4], Vec<u8>)>> = vec![
        vec![],
        vec![(b"knko", vec![1]), (b"infx", vec![]), (b"clbl", vec![0, 9])],
        vec![(b"knko", vec![7, 0, 0, 0]), (b"tsly", vec![0]), (b"lmgm", vec![2]), (b"vmgm", vec![1])],
    ];
    for (i, fs) in flag_sets.iter().enumerate() {
        let a = advanced_from_blocks(|k| fs.iter().find(|(fk, _)| *fk == k).map(|(_, d)| d.as_slice()));
        let enc = fs.iter().map(|(k, d)| format!("{}:{}", hexs(*k), hexs(d))).collect::<Vec<_>>().join(",");
        println!("adv\tget\t{i}\t{enc}\t{}", js!(&a));
    }
    let mut full = BlendingRanges::full(3).data;
    full[..4].copy_from_slice(&[10, 40, 255, 255]);
    full[3 * 8 + 4..3 * 8 + 8].copy_from_slice(&[0, 0, 200, 200]);
    for d in [vec![], BlendingRanges::full(3).data, full.clone(), full[..20].to_vec(), vec![1, 2, 3, 4, 5, 6, 7, 8, 9]] {
        let b = blend_if_from_ranges(&BlendingRanges { data: d.clone() });
        println!("bif\tp\t{}\t{}", b64(&d), js!(&b));
    }
    let bifs = vec![
        photocraft_doc::BlendIf::default(),
        blend_if_from_ranges(&BlendingRanges { data: full.clone() }),
        {
            let mut s = photocraft_doc::BlendIf::default();
            s.set(0, [photocraft_doc::BlendRange { black: [5, 5], white: [255, 255] }, photocraft_doc::BlendRange::FULL]);
            s
        },
        {
            let mut s = photocraft_doc::BlendIf::default();
            s.set(6, [photocraft_doc::BlendRange::FULL, photocraft_doc::BlendRange { black: [1, 2], white: [3, 4] }]);
            s
        },
    ];
    for b in &bifs {
        for ch in [1usize, 3, 4] {
            println!("bif\tw\t{}\t{ch}\t{}", js!(b), b64(&ranges_from_blend_if(b, ch).data));
        }
    }
    for bits in [0u32, 1, 2, 4, 8, 0x0D, 0x10, 0x1F, 0x8000_0000, 0x8000_000F, u32::MAX] {
        let l = locks_from_lspf(bits);
        println!("lspf\t{bits}\t{}\t{}", js!(&l), lspf_from_locks(&l));
    }
    for i in 0..14u16 {
        let l = label_from_index(i);
        println!("lbl\t{i}\t{}\t{}", js!(&l), label_index(l));
    }
    let mut smart_cases: Vec<(String, [u8; 4], Vec<u8>)> = vec![
        ("affine".into(), *b"SoLd", sold(placed_desc([10.0, 20.0, 110.0, 20.0, 110.0, 70.0, 10.0, 70.0], None, None))),
        ("rotated".into(), *b"SoLE", sold(placed_desc([10.0, 20.0, 60.0, 70.0, 10.0, 120.0, -40.0, 70.0], None, None))),
        ("persp".into(), *b"SoLd", sold(placed_desc([0.0, 0.0, 100.0, 10.0, 90.0, 60.0, 5.0, 50.0], None, None))),
        ("nonaffine".into(), *b"SoLd", sold(placed_desc([0.0, 0.0, 50.0, 0.0, 50.0, 25.0, 0.0, 25.0], Some([0.0, 0.0, 60.0, 5.0, 55.0, 30.0, -5.0, 20.0]), None))),
        ("nan".into(), *b"SoLd", sold(placed_desc([0.0, f64::NAN, 50.0, 0.0, 50.0, 25.0, 0.0, 26.0], None, None))),
        ("plld".into(), *b"PlLd", sold(placed_desc([10.0, 20.0, 110.0, 20.0, 110.0, 70.0, 10.0, 70.0], None, None))),
        ("short".into(), *b"SoLd", vec![1, 2, 3]),
        ("arc".into(), *b"SoLd", sold(placed_desc([0.0; 8], None, Some(warp_desc("warpArc", None))))),
        ("none".into(), *b"SoLd", sold(placed_desc([0.0; 8], None, Some(warp_desc("warpNone", None))))),
        ("custom".into(), *b"SoLE", sold(placed_desc([0.0; 8], None, Some(warp_desc("warpCustom", Some(mesh16(7.0))))))),
        ("custom-id".into(), *b"SoLE", sold(placed_desc([0.0; 8], None, Some(warp_desc("warpCustom", Some(mesh16(0.0))))))),
        ("custom-short".into(), *b"SoLE", sold(placed_desc([0.0; 8], None, Some(warp_desc("warpCustom", Some((vec![1.0; 15], vec![1.0; 16]))))))),
        ("custom-nomesh".into(), *b"SoLE", sold(placed_desc([0.0; 8], None, Some(warp_desc("warpCustom", None))))),
        ("badstyle".into(), *b"SoLE", sold(placed_desc([0.0; 8], None, Some(warp_desc("warpWhatever", None))))),
        ("spaced".into(), *b"SoLE", sold(placed_desc([0.0; 8], None, Some(warp_desc(" warp Flag ", None))))),
    ];
    let mut trailing = sold(placed_desc([10.0, 20.0, 110.0, 20.0, 110.0, 70.0, 10.0, 70.0], None, None));
    trailing.extend_from_slice(&[0, 0]);
    smart_cases.push(("trailing".into(), *b"SoLd", trailing));
    for (name, k, data) in smart_cases {
        let (id, a) = parse_smart(&k, &data);
        println!("smart\t{name}\t{}\t{}\t{id:?}\t{}", hexs(&k), b64(&data), js!(&a));
        let p = parse_smart_perspective(&k, &data).map_or("-".into(), |m| m.iter().map(|v| dbits(*v)).collect::<Vec<_>>().join(","));
        println!("persp\t{name}\t{p}");
        println!("warp\t{name}\t{}", warp_s(&parse_placed_warp(&k, &data)));
    }
    let mut ty = vec![0u8; 50];
    ty[2..10].copy_from_slice(&2.0f64.to_be_bytes());
    ty.extend_from_slice(&[0, 50]);
    ty.extend(vd(Descriptor::new("TxLr").with("Txt ", txt("Hello\u{0}x"))));
    for (name, d) in [("short", vec![0u8; 10]), ("full", ty.clone()), ("nodesc", ty[..52].to_vec()), ("real", tysh_for("Real text")), ("cut51", ty[..51].to_vec())] {
        let r = parse_tysh(&d).map_or("none".into(), |(t, a)| format!("{t:?} {}", js!(&a)));
        println!("tysh\t{name}\t{}\t{r}", b64(&d));
    }
    let off = lfx2_desc(Descriptor::new("null").with("masterFXSwitch", Value::Boolean(false)));
    for (name, d) in [("off", off), ("plain", vec![0, 0, 0, 0]), ("short", vec![0]), ("on", lfx2_desc(Descriptor::new("null").with("masterFXSwitch", Value::Boolean(true))))] {
        println!("fxen\t{name}\t{}\t{}", b64(&d), u8::from(effects_enabled(&d)));
    }
}

// ---------------------------------------------------------------------------------------------
// adjustments
// ---------------------------------------------------------------------------------------------

fn i16s(v: &[i16]) -> Vec<u8> {
    v.iter().flat_map(|x| x.to_be_bytes()).collect()
}

fn adj_cases() -> Vec<([u8; 4], Vec<u8>, Option<Vec<u8>>)> {
    let mut v: Vec<([u8; 4], Vec<u8>, Option<Vec<u8>>)> = Vec::new();
    // Arbitrary bytes under every key (the reference's never-panic loop), plus truncations.
    for key in photocraft_io::ADJUSTMENT_KEYS {
        for len in [0usize, 1, 3, 7, 19, 40, 63] {
            let d: Vec<u8> = (0..len).map(|i| (i * 37 + len) as u8).collect();
            v.push((*key, d.clone(), if len % 2 == 1 { Some(d) } else { None }));
        }
    }
    let mut levl = vec![0, 2];
    for r in 0..29u16 {
        for x in [r * 3, 255 - r, r, 250 - r * 2, 100 + r * 5] {
            levl.extend_from_slice(&x.to_be_bytes());
        }
    }
    v.push((*b"levl", levl.clone(), None));
    v.push((*b"levl", levl[..44].to_vec(), None));
    let mut curv = vec![0, 0, 1, 0, 0, 0, 0x3f];
    for bit in 0..6u16 {
        curv.extend_from_slice(&(3u16).to_be_bytes());
        for p in 0..3u16 {
            curv.extend_from_slice(&(p * 100 + bit).to_be_bytes());
            curv.extend_from_slice(&(p * 120).to_be_bytes());
        }
    }
    v.push((*b"curv", curv.clone(), None));
    let mut curv4 = curv.clone();
    curv4[2] = 4;
    v.push((*b"curv", curv4, None));
    let mut ident_black = vec![0, 0, 1, 0, 0, 0, 0x11];
    for _ in 0..2 {
        ident_black.extend_from_slice(&[0, 2, 0, 0, 0, 0, 0, 255, 0, 255]);
    }
    v.push((*b"curv", ident_black, None));
    let mut hue = vec![0, 2, 0, 0];
    hue.extend(i16s(&[10, 20, 30, -40, 50, -60]));
    for r in 0..6i16 {
        hue.extend(i16s(&[r * 60 - 20, r * 60 - 5, r * 60 + 5, r * 60 + 400, r * 3, -r * 4, r]));
    }
    v.push((*b"hue2", hue.clone(), None));
    let mut huec = hue.clone();
    huec[2] = 1;
    v.push((*b"hue2", huec, None));
    v.push((*b"hue2", hue[..30].to_vec(), None));
    let cged = vd(Descriptor::new("null").with("Brgh", Value::Integer(-20)).with("Cntr", uf(b"#Prc", 33.0)).with("useLegacy", Value::Boolean(true)));
    v.push((*b"brit", vec![0, 10, 0xff, 0xf6, 0, 127, 0], Some(cged)));
    v.push((*b"brit", vec![0, 10, 0xff, 0xf6, 0, 127, 0], Some(vd(Descriptor::new("null").with("Brgh", Value::Integer(5))))));
    v.push((*b"brit", vec![0, 10, 0xff, 0xf6, 0, 127, 0], Some(vec![1, 2, 3])));
    let mut exp = vec![0, 1];
    for x in [1.5f32, -0.01, 0.9] {
        exp.extend_from_slice(&x.to_be_bytes());
    }
    v.push((*b"expA", exp, None));
    v.push((*b"thrs", vec![0, 128, 0, 0], None));
    v.push((*b"post", vec![0, 4, 0, 0], None));
    v.push((*b"post", vec![255, 255], None));
    let mut selc = vec![0, 1, 0, 1];
    selc.extend([0u8; 8]);
    for r in 0..9i16 {
        selc.extend(i16s(&[r * 10 - 40, 5, -100, 100]));
    }
    v.push((*b"selc", selc.clone(), None));
    v.push((*b"selc", selc[..83].to_vec(), None));
    let cube = b"TITLE \"t\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    for (fmt, data, name) in [("LUTFormatCUBE", cube.to_vec(), Some("Look.cube")), ("LUTFormat3DL", b"0 512 1023\n".to_vec(), None), ("LUTFormatLOOK", b"<look/>".to_vec(), None)] {
        let mut d = Descriptor::new("null").with("lookupType", en("colorLookupType", "3DLUT")).with("NM  ", txt("Id")).with("Dthr", Value::Boolean(true)).with("LUTFormat", en("LUTFormatType", fmt)).with("LUT3DFileData", Value::RawData(data));
        if let Some(n) = name {
            d = d.with("LUT3DFileName", txt(n));
        }
        let mut b = vec![0, 1];
        b.extend(vd(d));
        v.push((*b"clrL", b, None));
    }
    let mut prof = vec![0, 1];
    prof.extend(vd(Descriptor::new("null").with("lookupType", en("colorLookupType", "abstractProfile")).with("LUT3DFileData", Value::RawData(vec![]))));
    v.push((*b"clrL", prof, None));
    let grdm = |version: u16, method: &[u8], stops: &[(u32, u32, [u16; 3])], smooth: u16| {
        let mut d = Vec::new();
        d.extend_from_slice(&version.to_be_bytes());
        d.extend_from_slice(&[1, 1]);
        d.extend_from_slice(method);
        d.extend_from_slice(&3u32.to_be_bytes());
        d.extend(i16s(&[0x47, 0x31, 0]));
        d.extend_from_slice(&(stops.len() as u16).to_be_bytes());
        for (loc, mid, c) in stops {
            d.extend_from_slice(&loc.to_be_bytes());
            d.extend_from_slice(&mid.to_be_bytes());
            d.extend_from_slice(&[0, 0]);
            for x in c {
                d.extend_from_slice(&x.to_be_bytes());
            }
            d.extend_from_slice(&[0, 0, 0, 0]);
        }
        d.extend_from_slice(&1u16.to_be_bytes());
        d.extend_from_slice(&[0, 0, 0, 0, 0, 0, 0, 50, 0, 255]);
        d.extend_from_slice(&[0, 2]);
        d.extend_from_slice(&smooth.to_be_bytes());
        d.extend_from_slice(&[0; 20]);
        d
    };
    let stops = [(4096u32, 50u32, [65535u16, 0, 0]), (0, 30, [0, 0, 65535]), (2048, 70, [0, 32768, 0])];
    v.push((*b"grdm", grdm(1, &[], &stops, 0), None));
    v.push((*b"grdm", grdm(1, &[], &stops, 4096), None));
    v.push((*b"grdm", grdm(3, b"Perc", &stops, 2048), None));
    v.push((*b"grdm", grdm(3, b"Lnr ", &stops[..2], 0), None));
    v.push((*b"grdm", grdm(1, &[], &stops[..1], 0), None));
    v.push((*b"grdm", grdm(2, &[], &stops, 0), None));
    v.push((*b"vibA", vd(Descriptor::new("null").with("vibrance", Value::Integer(-40)).with("Strt", Value::Integer(250))), None));
    v.push((*b"vibA", vd(Descriptor::new("null").with("Strt", Value::Double(10.5))), None));
    let mut b = i16s(&[-30, 20, 0, 10, -5, 100, 0, 0, -300]);
    b.extend([1, 0]);
    v.push((*b"blnc", b.clone(), None));
    v.push((*b"blnc", b[..19].to_vec(), None));
    let tint = Descriptor::new("RGBC").with("Rd  ", Value::Double(255.0)).with("Grn ", Value::Double(127.5)).with("Bl  ", Value::Double(0.0));
    let mut bw = Descriptor::new("null");
    for (k, w) in ["Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"].iter().zip([10, 20, 30, 400, -500, 60]) {
        bw = bw.with(k, Value::Integer(w));
    }
    v.push((*b"blwh", vd(bw.clone().with("useTint", Value::Boolean(true)).with("tintColor", Value::Descriptor(tint.clone()))), None));
    v.push((*b"blwh", vd(bw.clone().with("useTint", Value::Boolean(true)).with("tintColor", Value::Descriptor(Descriptor::new("Grsc").with("Gry ", Value::Double(25.0))))), None));
    v.push((*b"blwh", vd(bw.with("useTint", Value::Boolean(true))), None));
    v.push((*b"blwh", vd(Descriptor::new("null").with("useTint", Value::Boolean(false)).with("tintColor", Value::Descriptor(tint))), None));
    let mut p2 = i16s(&[2, 0]);
    p2.extend([0xff, 0xff, 0x80, 0x00, 0, 0, 0, 0]);
    p2.extend(40u32.to_be_bytes());
    p2.extend([1, 0, 0, 0]);
    v.push((*b"phfl", p2.clone(), None));
    v.push((*b"phfl", p2[..18].to_vec(), None));
    let mut lab = i16s(&[2, 7, 5000, 0, 0, 0]);
    lab.extend(25u32.to_be_bytes());
    lab.push(0);
    v.push((*b"phfl", lab, None));
    let mut cmyk = i16s(&[2, 2, 0, 0, 0, 0]);
    cmyk.extend(25u32.to_be_bytes());
    cmyk.push(0);
    v.push((*b"phfl", cmyk, None));
    for (l, a, bb) in [(10000i32, 0i32, 0i32), (5000, -6000, 0), (20000, 0, 0), (5000, 0, -13000)] {
        let mut v3 = vec![0, 3];
        for x in [l, a, bb] {
            v3.extend(x.to_be_bytes());
        }
        v3.extend(100u32.to_be_bytes());
        v3.push(1);
        v.push((*b"phfl", v3, None));
    }
    let mut mixr = i16s(&[1, 0]);
    mixr.extend(i16s(&[100, 0, 0, 0, 20, 50, 50, 0, 0, 0, 0, -50, 150, 0, -10]));
    v.push((*b"mixr", mixr.clone(), None));
    let mut mono = i16s(&[1, 1]);
    mono.extend(i16s(&[30, 59, 11, 0, 0]));
    v.push((*b"mixr", mono, None));
    let mut cm = i16s(&[1, 0]);
    for r in 0..4 {
        cm.extend(i16s(&std::array::from_fn::<i16, 5, _>(|k| if k == r { 100 } else { 0 })));
    }
    v.push((*b"mixr", cm, None));
    v.push((*b"mixr", vec![0, 1, 0, 0, 0, 100], None));
    v.push((*b"nvrt", vec![], None));
    v
}

fn adjustments() {
    let modes = [(ColorMode::Rgb, "rgb"), (ColorMode::Grayscale, "gray"), (ColorMode::Cmyk, "cmyk"), (ColorMode::Lab, "lab")];
    for (key, data, cged) in adj_cases() {
        for (mode, mname) in modes {
            let mut r = empty_rec("adj");
            blk(&mut r, &key, data.clone());
            if let Some(c) = &cged {
                blk(&mut r, b"CgEd", c.clone());
            }
            let f = file_with(pmode(mode), 8, 2, 2, vec![r]);
            let (doc, _) = photocraft_io::psd_to_document(&f);
            let res = match doc.layers.first().map(|l| &l.content) {
                Some(LayerContent::Adjustment(a)) => js!(a),
                _ => "?".into(),
            };
            println!("adj\t{}\t{mname}\t{}\t{}\t{res}", hexs(&key), b64(&data), cged.as_ref().map_or("-".into(), |c| b64(c)));
        }
    }
    // Writer: every kind (io_common::gen_doc's list plus the reference's round-trip cases).
    use photocraft_doc::Adjustment;
    use photocraft_doc::adjust::{CurvePoint, HueRange, LevelsChannel, ToneSpace};
    let lc = |a: u16, b: u16| LevelsChannel { in_black: f32::from(a) / 255.0, in_white: f32::from(b) / 255.0, gamma: 1.2, out_black: 0.0, out_white: 1.0 };
    let pts = |v: &[(u8, u8)]| v.iter().map(|&(i, o)| CurvePoint { input: f32::from(i) / 255.0, output: f32::from(o) / 255.0 }).collect::<Vec<_>>();
    let mut ranges = HueRange::defaults();
    ranges[0] = HueRange { hue: 15.0, saturation: -40.0, lightness: 10.0, bounds: [-60.0, -20.0, 10.0, 30.0] };
    ranges[4].saturation = 25.0;
    let id = photocraft_cms::lutfile::LutFile::identity(5);
    let adjs = vec![
        Adjustment::Invert,
        Adjustment::Threshold { level: 128.0 / 255.0 },
        Adjustment::Threshold { level: 0.0 },
        Adjustment::Posterize { levels: 4 },
        Adjustment::Posterize { levels: 1000 },
        Adjustment::BrightnessContrast { brightness: 20.0, contrast: -10.0, legacy: false },
        Adjustment::BrightnessContrast { brightness: -150.5, contrast: 1e9, legacy: true },
        Adjustment::HueSaturation { hue: 30.0, saturation: -20.0, lightness: 5.0, colorize: false, ranges: HueRange::defaults() },
        Adjustment::HueSaturation { hue: -10.0, saturation: 0.0, lightness: 0.0, colorize: true, ranges },
        Adjustment::Exposure { exposure: 1.5, offset: -0.01, gamma: 0.9 },
        Adjustment::Levels { master: lc(10, 240), per_channel: [lc(0, 255), lc(5, 250), lc(20, 200)], space: ToneSpace::Rgb, black: LevelsChannel::default() },
        Adjustment::Levels { master: lc(0, 255), per_channel: [lc(0, 255), lc(5, 250), lc(0, 255)], space: ToneSpace::Cmyk, black: lc(30, 255) },
        Adjustment::Curves {
            master: pts(&[(0, 0), (128, 150), (255, 255)]),
            per_channel: [pts(&[(0, 10), (255, 255)]), pts(&[(0, 0), (255, 245)]), pts(&[(0, 0), (64, 32), (255, 255)])],
            space: ToneSpace::Rgb,
            black: Vec::new(),
            pencil: false,
        },
        Adjustment::Curves {
            master: pts(&[(0, 0), (255, 255)]),
            per_channel: [pts(&[(0, 0), (255, 255)]), pts(&[(0, 30), (255, 255)]), pts(&[(0, 0), (255, 255)])],
            space: ToneSpace::Cmyk,
            black: pts(&[(0, 0), (128, 100), (255, 255)]),
            pencil: false,
        },
        Adjustment::Curves { master: (0..25).map(|i| CurvePoint { input: i as f32 / 24.0, output: 1.0 - i as f32 / 24.0 }).collect(), per_channel: Default::default(), space: ToneSpace::Lab, black: Vec::new(), pencil: false },
        Adjustment::SelectiveColor { relative: true, adjustments: std::array::from_fn(|r| [r as f32 * 10.0 - 40.0, 5.0, -100.0, 300.0]) },
        Adjustment::ColorLookup { name: "Look.cube".into(), lut: Some(Arc::new(id.data)), size: 5, tetrahedral: false, dither: true },
        Adjustment::ColorLookup { name: "L".into(), lut: None, size: 3, tetrahedral: true, dither: true },
        Adjustment::ColorLookup { name: "L".into(), lut: Some(Arc::new(vec![0.5; 7])), size: 3, tetrahedral: false, dither: true },
        Adjustment::GradientMap { stops: vec![(0.0, [0.0, 0.0, 0.0]), (0.5, [1.0, 0.0, 0.0]), (1.0, [1.0, 1.0, 1.0])], reverse: true, dither: false },
        Adjustment::GradientMap { stops: vec![], reverse: false, dither: true },
        Adjustment::GradientMap { stops: vec![(0.3, [0.2, 0.4, 0.6])], reverse: true, dither: false },
        Adjustment::Vibrance { vibrance: 35.0, saturation: -120.0 },
        Adjustment::ColorBalance { shadows: [20.0, -10.0, 5.0], midtones: [-15.0, 10.0, 30.0], highlights: [0.0, 5.0, -100.0], preserve_luminosity: true },
        Adjustment::BlackWhite { weights: [-200.0, 300.0, 0.0, 10.0, 90.0, 30.0], tint: Some([0.9, 0.7, 0.5]) },
        Adjustment::BlackWhite { weights: [40.0, 60.0, 40.0, 60.0, 20.0, 80.0], tint: None },
        Adjustment::PhotoFilter { color: [0.8, 0.4, 0.2], density: 0.14, preserve_luminosity: true },
        Adjustment::ChannelMixer { matrix: [[0.5, 0.3, 0.2, 0.0], [0.1, 0.8, 0.1, 0.05], [0.0, 0.2, 0.9, -0.05]], monochrome: false },
        Adjustment::ChannelMixer { matrix: [[0.4, 0.4, 0.2, 0.1], [0.0, 1.0, 0.0, 0.0], [-2.0, 0.0, 2.0, 0.0]], monochrome: true },
        Adjustment::Unsupported { psd_key: "selc".into(), raw: vec![1, 2, 3] },
        Adjustment::Unsupported { psd_key: "abc".into(), raw: vec![] },
    ];
    for a in adjs {
        let mut d = Document::new("w", Size::new(2, 2), ColorMode::Rgb, SampleType::U8);
        d.layers.push(Layer::new("a", LayerContent::Adjustment(a.clone())));
        let f = photocraft_io::document_to_psd(&d);
        let keys: Vec<&[u8; 4]> = photocraft_io::ADJUSTMENT_KEYS.iter().copied().chain([b"CgEd"]).collect();
        let blocks = f
            .layers()
            .first()
            .map(|r| r.blocks.iter().filter(|b| keys.contains(&&b.key)).map(|b| format!("{}:{}", hexs(&b.key), b64(&b.data))).collect::<Vec<_>>().join(","))
            .unwrap_or_default();
        println!("wadj\t{}\t{}", js!(&a), blocks);
    }
}

// ---------------------------------------------------------------------------------------------
// synthetic files
// ---------------------------------------------------------------------------------------------

fn res(id: u16, data: Vec<u8>) -> ImageResource {
    ImageResource::new(id, data)
}

fn guides_res(list: &[(i32, u8)]) -> Vec<u8> {
    let mut d = vec![0, 0, 0, 1, 0, 0, 2, 64, 0, 0, 2, 64];
    d.extend_from_slice(&(list.len() as u32).to_be_bytes());
    for (pos, dir) in list {
        d.extend_from_slice(&pos.to_be_bytes());
        d.push(*dir);
    }
    d
}

fn names_res(names: &[&str]) -> Vec<u8> {
    let mut d = Vec::new();
    for n in names {
        let u: Vec<u16> = n.encode_utf16().chain([0]).collect();
        d.extend_from_slice(&(u.len() as u32).to_be_bytes());
        for x in u {
            d.extend_from_slice(&x.to_be_bytes());
        }
    }
    d
}

/// A layered RGB 8-bit file built from records with the given extra resources / global blocks.
fn rgb_file(recs: Vec<LayerRecord>, resources: Vec<ImageResource>, globals: Vec<TaggedBlock>) -> PsdFile {
    let mut f = file_with(PMode::Rgb, 8, 16, 12, recs);
    f.resources.extend(resources);
    f.global_blocks = globals;
    f
}

fn synth_files() -> Vec<(String, PsdFile)> {
    let mut v: Vec<(String, PsdFile)> = Vec::new();
    let r = |x: i32, y: i32, w: u32, h: u32| PRect::from_xywh(x, y, w, h);
    // Plain raster layers: offsets, negative positions, empty rect, bare rect, missing channels.
    {
        let mut a = rgb_rec("A", r(1, 2, 5, 4), 1);
        a.opacity = 200;
        a.clipping = 1;
        a.flags = LayerFlags(2 | 1);
        let neg = rgb_rec("Neg \u{e9}", r(-3, -2, 6, 5), 2);
        let empty = empty_rec("Empty");
        let bare = rec("Bare", r(0, 0, 300000, 300000), PMode::Rgb, 8, &[], 0);
        let noalpha = rec("NoAlpha", r(2, 2, 3, 3), PMode::Rgb, 8, &[0, 1, 2], 4);
        let onlyalpha = rec("OnlyAlpha", r(2, 2, 3, 3), PMode::Rgb, 8, &[-1], 5);
        let mut badc = rgb_rec("BadChannel", r(0, 0, 4, 4), 6);
        badc.channels[1].compression = Some(Compression::Rle);
        badc.channels[1].data = vec![0, 1, 2];
        let mut unk = rgb_rec("UnknownBlend", r(0, 0, 2, 2), 7);
        unk.blend_mode = photocraft_psd::BlendMode::from_key(*b"zzzz");
        v.push(("raster".into(), rgb_file(vec![a, neg, empty, bare, noalpha, onlyalpha, badc, unk], vec![], vec![])));
    }
    // Masks: user mask, real mask, parameters, disabled/unlinked, flag 3 on a rendered layer.
    {
        let mk = |name: &str, m: PsdMask, chans: &[i16]| {
            let mut x = rec(name, r(0, 0, 8, 6), PMode::Rgb, 8, chans, 9);
            x.mask = MaskData::Mask(m.clone());
            for &id in chans.iter().filter(|&&i| i < -1) {
                let mr = if id == -3 { m.real.map_or(m.rect, |rm| rm.rect) } else { m.rect };
                let (w, h) = mr.size().unwrap_or((0, 0));
                if let Ok(c) = ChannelData::encode(id, Compression::Raw, &photocraft_psd::testgen::pattern_plane(w, h, 8, 20), w, h, 8, Version::Psd) {
                    x.channels.retain(|c| c.id != id);
                    x.channels.push(c);
                }
            }
            x
        };
        let base = |flags: u8| PsdMask { rect: r(1, 1, 4, 3), default_color: 255, flags, parameters: None, real: None, trailing: vec![0, 0], real_first: true };
        let params = MaskParameters { flags: 0x0f, user_density: Some(128), user_feather: Some(2.5), vector_density: Some(200), vector_feather: Some(1.25) };
        let real = RealMask { flags: 1, background: 0, rect: r(2, 2, 3, 2) };
        let ms = vec![
            mk("user", base(0), &[-1, 0, 1, 2, -2]),
            mk("disabled", base(2 | 1), &[-1, 0, 1, 2, -2]),
            mk("params", PsdMask { parameters: Some(params), flags: 16, ..base(16) }, &[-1, 0, 1, 2, -2]),
            mk("real", PsdMask { real: Some(real), ..base(0) }, &[-1, 0, 1, 2, -2, -3]),
            mk("realmeta", PsdMask { real: Some(real), ..base(0) }, &[-1, 0, 1, 2, -2]),
            mk("nochan", base(0), &[-1, 0, 1, 2]),
            mk("emptyrect", PsdMask { rect: PRect::default(), default_color: 0, ..base(8) }, &[-1, 0, 1, 2, -2]),
        ];
        v.push(("masks".into(), rgb_file(ms, vec![], vec![])));
    }
    // Blending options: lspf, lclr, iOpa, fxrp, brst, advanced, Blend If, lyid, bad typed blocks.
    {
        let mut a = rgb_rec("Options", r(0, 0, 3, 3), 11);
        a.blocks.push(TaggedBlock::protection(0x8000_000D));
        a.blocks.push(TaggedBlock::sheet_color(9));
        a.blocks.push(TaggedBlock::fill_opacity(77));
        a.blocks.push(TaggedBlock::layer_id(4242));
        a.blocks.push(TaggedBlock::knockout(2));
        a.blocks.push(TaggedBlock::blend_clipped_as_group(false));
        a.blocks.push(TaggedBlock::transparency_shapes(false));
        let mut fxrp = 12.5f64.to_be_bytes().to_vec();
        fxrp.extend_from_slice(&(-3.25f64).to_be_bytes());
        blk(&mut a, b"fxrp", fxrp);
        blk(&mut a, b"brst", vec![0, 0, 0, 1, 0, 0, 0, 40]);
        let mut br = BlendingRanges::full(3).data;
        br[..4].copy_from_slice(&[10, 40, 255, 255]);
        br[2 * 8 + 4..2 * 8 + 8].copy_from_slice(&[0, 0, 200, 210]);
        a.blending_ranges = BlendingRanges { data: br };
        let mut b = rgb_rec("BadTyped", r(0, 0, 2, 2), 12);
        blk(&mut b, b"lspf", vec![1]);
        blk(&mut b, b"lclr", vec![]);
        blk(&mut b, b"fxrp", vec![1, 2, 3]);
        blk(&mut b, b"knko", vec![]);
        b.flags = LayerFlags(1);
        b.blending_ranges = BlendingRanges { data: vec![0, 0, 255, 255, 0, 0, 255, 255, 1] };
        v.push(("options".into(), rgb_file(vec![a, b], vec![res(1026, vec![0, 3, 0, 0, 0, 9])], vec![])));
    }
    // Effects: lfx2, lfxs on a group, lmfx superseding, lrFX on a layer and a group, bad lfx2.
    {
        let fx = sample_effects();
        let mut a = rgb_rec("lfx2", r(0, 0, 3, 3), 13);
        blk(&mut a, b"lfx2", photocraft_io::effects_map::write_lfx2(false, &fx));
        let mut b = rgb_rec("lmfx", r(0, 0, 3, 3), 14);
        blk(&mut b, b"lfx2", photocraft_io::effects_map::write_lfx2(true, &fx[..2]));
        blk(&mut b, b"lmfx", photocraft_io::effects_map::write_lfx2(true, &fx[5..]));
        let mut c = rgb_rec("badlfx2", r(0, 0, 3, 3), 15);
        blk(&mut c, b"lfx2", lfx2_desc(Descriptor::new("null").with("masterFXSwitch", Value::Boolean(false))).into_iter().take(30).collect());
        let mut d = rgb_rec("lrFX", r(0, 0, 3, 3), 16);
        blk(&mut d, b"lrFX", lrfx_cases()[0].1.clone());
        let mut e = rgb_rec("lrFXbad", r(0, 0, 3, 3), 17);
        blk(&mut e, b"lrFX", vec![0, 0, 0, 1, b'X']);
        let child = rgb_rec("child", r(1, 1, 2, 2), 18);
        let mut recs = vec![a, b, c, d, e];
        let mut g = group_recs("FX group", vec![child], SectionType::OpenFolder, None);
        if let Some(last) = g.last_mut() {
            blk(last, b"lfxs", photocraft_io::effects_map::write_lfx2(true, &fx[9..11]));
            blk(last, b"lrFX", lrfx_cases()[0].1.clone());
        }
        recs.extend(g);
        let mut g2 = group_recs("lrFX group", vec![], SectionType::ClosedFolder, Some(photocraft_psd::BlendMode::Multiply));
        if let Some(last) = g2.last_mut() {
            blk(last, b"lrFX", lrfx_cases()[0].1.clone());
        }
        recs.extend(g2);
        v.push(("effects".into(), rgb_file(recs, vec![], vec![])));
    }
    // Groups: nesting, closed, blend override, unknown divider blend, unbalanced, link groups.
    {
        let inner = group_recs("Inner", vec![rgb_rec("deep", r(0, 0, 2, 2), 19)], SectionType::ClosedFolder, Some(photocraft_psd::BlendMode::Screen));
        let mut outer = group_recs("Outer", inner, SectionType::OpenFolder, Some(photocraft_psd::BlendMode::from_key(*b"zzzz")));
        outer.push(rgb_rec("top", r(0, 0, 2, 2), 20));
        let mut stray = empty_rec("stray folder");
        stray.blocks.push(TaggedBlock::section_divider(SectionType::OpenFolder, None, None));
        outer.push(stray);
        let n = outer.len();
        let ids: Vec<u8> = (0..n).flat_map(|i| (if i % 2 == 0 { 0u16 } else { (i % 3) as u16 + 1 }).to_be_bytes()).collect();
        v.push(("groups".into(), rgb_file(outer, vec![res(1026, ids)], vec![])));
    }
    // Groups nested deeper than the document's cap.
    {
        let mut recs = vec![rgb_rec("leaf", r(0, 0, 1, 1), 21)];
        for i in 0..102 {
            recs = group_recs(&format!("g{i}"), recs, SectionType::OpenFolder, None);
        }
        v.push(("deep-groups".into(), rgb_file(recs, vec![res(1026, vec![0, 1, 0, 2])], vec![])));
    }
    // Adjustment and fill layers (with CgEd, unreadable fills, a fill cache, a soft shape fill).
    {
        let mut recs = Vec::new();
        for (key, data, cged) in adj_cases().into_iter().rev().take(40) {
            let mut a = empty_rec("adj");
            blk(&mut a, &key, data);
            if let Some(c) = cged {
                blk(&mut a, b"CgEd", c);
            }
            recs.push(a);
        }
        for (i, (_, k, data)) in fill_cases().into_iter().enumerate() {
            let mut f = if i % 3 == 0 { rgb_rec("fill", r(0, 0, 4, 4), 30 + i as u32) } else { empty_rec("fill") };
            blk(&mut f, &k, data);
            recs.push(f);
        }
        v.push(("adj-fill".into(), rgb_file(recs, vec![], vec![])));
    }
    // Text layers (TySh; with and without pixels; blank engine text; garbage TySh) and Txt2.
    {
        let mut a = rgb_rec("Text", r(2, 2, 6, 4), 40);
        blk(&mut a, b"TySh", tysh_for("Hello"));
        let mut b = empty_rec("Text no pixels");
        blk(&mut b, b"TySh", tysh_for("World"));
        let mut c = empty_rec("Blank");
        blk(&mut c, b"TySh", tysh_for("   "));
        let mut d = rgb_rec("Garbage TySh", r(0, 0, 2, 2), 41);
        let mut g = vec![0u8; 50];
        g[2..10].copy_from_slice(&3.0f64.to_be_bytes());
        g.extend_from_slice(&[0, 50]);
        g.extend(vd(Descriptor::new("TxLr").with("Txt ", txt("fallback"))));
        blk(&mut d, b"TySh", g);
        let mut m = rgb_rec("Masked text", r(0, 0, 4, 4), 42);
        blk(&mut m, b"TySh", tysh_for("M"));
        m.mask = MaskData::Mask(PsdMask { rect: r(0, 0, 2, 2), default_color: 0, flags: 8, parameters: None, real: None, trailing: vec![0, 0], real_first: true });
        let mr = r(0, 0, 2, 2);
        if let Ok(ch) = ChannelData::encode(-2, Compression::Raw, &[1, 2, 3, 4], 2, 2, 8, Version::Psd) {
            m.channels.push(ch);
        }
        let _ = mr;
        let txt2 = TaggedBlock::new(*b"Txt2", b"<< /DocumentResources << >> >>".to_vec());
        v.push(("text".into(), rgb_file(vec![a, b, c, d, m], vec![], vec![txt2])));
    }
    // Smart objects: SoLd affine / perspective / warp, PlLd, SoLE; FEid good and bad.
    {
        let mut a = rgb_rec("Smart", r(0, 0, 4, 4), 50);
        blk(&mut a, b"SoLd", sold(placed_desc([10.0, 20.0, 110.0, 20.0, 110.0, 70.0, 10.0, 70.0], None, Some(warp_desc("warpArc", None)))));
        let mut b = empty_rec("Persp");
        blk(&mut b, b"SoLE", sold(placed_desc([0.0, 0.0, 100.0, 10.0, 90.0, 60.0, 5.0, 50.0], None, None)));
        let mut c = rgb_rec("PlLd", r(0, 0, 2, 2), 51);
        blk(&mut c, b"PlLd", vec![0; 12]);
        let mut d = rgb_rec("Bad SoLd", r(0, 0, 2, 2), 52);
        blk(&mut d, b"SoLd", vec![1, 2, 3, 4, 5, 6, 7, 8, 9]);
        let feid = TaggedBlock::new(*b"FEid", vec![0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 9, 1]);
        v.push(("smart".into(), rgb_file(vec![a, b, c, d], vec![], vec![feid])));
    }
    // Resources: resolution (ppcm, unequal), ICC, XMP, EXIF, guides, global light, paths, work
    // path, clipping path, unknown and mapped resources, alpha names; global blocks.
    {
        let mut ri = ResolutionInfo::from_dpi(300.0);
        ri.h_res_unit = 2;
        ri.v_res_fixed = 150 << 16;
        let mut cp = vec![5];
        cp.extend_from_slice(b"Clip1");
        cp.extend_from_slice(&[0, 0, 0, 0]);
        let resources = vec![
            res(1005, ri.to_bytes()),
            res(1039, vec![1, 2, 3, 4, 5]),
            res(1060, b"<x:xmpmeta \xff/>".to_vec()),
            res(1058, b"MM\0*\0\0\0\x08".to_vec()),
            res(1032, guides_res(&[(5 * 32 + 16, 1), (-64, 0), (12 * 32, 0)])),
            res(1037, (-45i32).to_be_bytes().to_vec()),
            res(1049, 65i32.to_be_bytes().to_vec()),
            res(1037, vec![1]),
            res(2000, vec![0, 6, 0, 0, 0, 0]),
            res(2001, vec![1, 2, 3]),
            res(1025, vec![0, 6]),
            res(2999, cp),
            res(2999, vec![200, 1]),
            res(4000, vec![9, 9]),
            res(1036, vec![1, 2]),
            res(1057, vec![0, 0, 0, 1]),
            res(1045, names_res(&["Alpha One"])),
        ];
        let mut named = res(4001, vec![7]);
        named.name = b"name \xc3\xa9\xff".to_vec();
        let mut resources = resources;
        resources.push(named);
        let globals = vec![TaggedBlock::new(*b"Abcd", vec![1, 2]), TaggedBlock::new(*b"Anno", vec![0, 0]), TaggedBlock::new(*b"FXid", vec![0, 0, 0, 1])];
        v.push(("resources".into(), rgb_file(vec![rgb_rec("L", r(0, 0, 2, 2), 60)], resources, globals)));
    }
    // Merged-only files of every mode, flattened modes with layers, palettes, duotone data,
    // unknown mode, extra alpha channels with names / display info / quick mask.
    for (name, mode, depth) in [
        ("rgb8", PMode::Rgb, 8u16),
        ("rgb16", PMode::Rgb, 16),
        ("rgb32", PMode::Rgb, 32),
        ("gray16", PMode::Grayscale, 16),
        ("cmyk8", PMode::Cmyk, 8),
        ("lab16", PMode::Lab, 16),
        ("lab8", PMode::Lab, 8),
        ("bitmap", PMode::Bitmap, 1),
        ("indexed", PMode::Indexed, 8),
        ("duotone", PMode::Duotone, 8),
        ("multi8", PMode::Multichannel, 8),
        ("multi16", PMode::Multichannel, 16),
        ("gray1", PMode::Grayscale, 1),
    ] {
        let f = photocraft_psd::testgen::merged_only(Version::Psd, mode, depth, Compression::Rle, 9, 5);
        v.push((format!("merged-{name}"), f.clone()));
        let mut lf = f.clone();
        let cc = photocraft_psd::testgen::mode_channels(mode) as i16;
        let chans: Vec<i16> = (-1..cc).collect();
        lf.layer_info = Some(LayerInfo { merged_alpha: false, layers: vec![rec("layer", r(1, 1, 3, 2), mode, depth, &chans, 3)], padding: None });
        v.push((format!("layered-{name}"), lf));
    }
    {
        let mut f = photocraft_psd::testgen::merged_only(Version::Psd, PMode::Indexed, 8, Compression::Raw, 4, 3);
        f.color_mode_data.truncate(100);
        v.push(("indexed-short-palette".into(), f));
        let mut f = photocraft_psd::testgen::merged_only(Version::Psd, PMode::Rgb, 8, Compression::Raw, 4, 3);
        f.header.color_mode = PMode::Unknown(5);
        v.push(("unknown-mode".into(), f));
        let mut f = photocraft_psd::testgen::merged_only(Version::Psd, PMode::Rgb, 8, Compression::Raw, 4, 3);
        f.color_mode_data = vec![1, 2, 3];
        v.push(("rgb-cmd".into(), f));
    }
    for (name, mode, depth, extra, alpha) in [
        ("alpha-rgb8", PMode::Rgb, 8u16, 2u16, true),
        ("alpha-rgb16", PMode::Rgb, 16, 1, false),
        ("alpha-cmyk", PMode::Cmyk, 8, 1, true),
        ("alpha-lab16", PMode::Lab, 16, 2, false),
        ("alpha-gray32", PMode::Grayscale, 32, 1, true),
    ] {
        let base = photocraft_psd::testgen::mode_channels(mode);
        let chans = base + extra + u16::from(alpha);
        let mut f = photocraft_psd::testgen::merged_only(Version::Psd, mode, depth, Compression::Raw, 6, 4);
        f.header.channels = chans;
        let mut planes = Vec::new();
        for c in 0..chans {
            planes.extend(photocraft_psd::testgen::pattern_plane(6, 4, depth, u32::from(c) + 1));
        }
        f.image_data = photocraft_psd::ImageData::encode(Compression::Raw, &planes, &f.header).unwrap_or(f.image_data);
        if alpha {
            f.layer_info = Some(LayerInfo { merged_alpha: true, layers: vec![], padding: None });
        }
        let mut di = vec![0, 0, 0, 1];
        for k in 0..extra {
            di.extend_from_slice(&[0, 0, 0xff, 0xff, 0, 0, 0x80, 0, 0, 0, 0, 50 + k as u8, if k == 0 { 2 } else { 1 }, 0]);
        }
        f.resources.push(res(1077, di));
        f.resources.push(res(1045, names_res(&["Spot \u{1F600}"])));
        if extra > 1 {
            f.resources.push(res(1022, (base + u16::from(alpha) + 1).to_be_bytes().iter().copied().chain([1]).collect()));
        }
        v.push((name.into(), f.clone()));
        // With a layer: the merged image is still decoded for the extra channels.
        let cc = base as i16;
        let lchans: Vec<i16> = (-1..cc).collect();
        f.layer_info = Some(LayerInfo { merged_alpha: alpha, layers: vec![rec("x", r(0, 0, 2, 2), mode, depth, &lchans, 7)], padding: None });
        f.resources.retain(|r| r.id != 1077);
        f.resources.push(res(1007, vec![0, 0, 0xff, 0xff, 0, 0, 0x80, 0, 0, 0, 0, 30, 1, 0]));
        v.push((format!("{name}-layered"), f));
    }
    // Flattened (no layer records) with alpha: the merged image is un-matted.
    {
        let mut f = photocraft_psd::testgen::merged_only(Version::Psd, PMode::Rgb, 8, Compression::Raw, 300, 3);
        f.header.channels = 4;
        let mut planes = Vec::new();
        for c in 0..4u16 {
            planes.extend(photocraft_psd::testgen::pattern_plane(300, 3, 8, u32::from(c)));
        }
        f.image_data = photocraft_psd::ImageData::encode(Compression::Raw, &planes, &f.header).unwrap_or(f.image_data);
        f.layer_info = Some(LayerInfo { merged_alpha: true, layers: vec![], padding: None });
        v.push(("flat-alpha".into(), f));
        let mut f = photocraft_psd::testgen::merged_only(Version::Psd, PMode::Rgb, 8, Compression::Raw, 4, 3);
        f.image_data.data.truncate(5);
        v.push(("merged-short".into(), f));
    }
    // Documents exported by the reference (rich layers, masks, groups, adjustments, fills, extras).
    for (mode, depth) in [
        (ColorMode::Rgb, SampleType::U8),
        (ColorMode::Rgb, SampleType::U16),
        (ColorMode::Rgb, SampleType::F32),
        (ColorMode::Grayscale, SampleType::U8),
        (ColorMode::Cmyk, SampleType::U8),
        (ColorMode::Cmyk, SampleType::U16),
        (ColorMode::Lab, SampleType::U8),
        (ColorMode::Lab, SampleType::U16),
    ] {
        let d = io_common::gen_doc(mode, depth, io_common::Features::ALL);
        v.push((format!("gen-{mode:?}-{depth:?}"), photocraft_io::document_to_psd(&d)));
    }
    {
        // Shape, text, smart, artboard, comps-free extras, exported then re-imported.
        let mut d = Document::new("rich", Size::new(32, 24), ColorMode::Rgb, SampleType::U8);
        let fmt = d.pixel_format();
        let sh = photocraft_doc::ShapeLayer { path: photocraft_vector::shapes::rect(2.0, 3.0, 10.0, 8.0), fill: Some(Fill::Solid(Color::rgb(0.0, 0.5, 1.0))), ..Default::default() };
        d.layers.push(Layer::new("Shape", LayerContent::Shape(sh)));
        let t = photocraft_doc::TextLayer { text: "Hi".into(), font_family: "Arial".into(), size_pt: 12.0, ..Default::default() };
        d.layers.push(Layer::new("Type", LayerContent::Text(t)));
        let mut vm = io_common::raster("Vector masked", fmt, photocraft_geom::Rect::new(0, 0, 10, 10), 3, true);
        vm.vector_mask = Some(photocraft_doc::VectorMask::new(photocraft_vector::shapes::ellipse(1.0, 1.0, 8.0, 8.0)));
        d.layers.push(vm);
        let child = io_common::raster("In board", fmt, photocraft_geom::Rect::new(4, 4, 8, 8), 4, true);
        let mut board = Layer::group("Board", vec![child]);
        if let LayerContent::Group(g) = &mut board.content {
            g.artboard = Some(photocraft_doc::Artboard::new(photocraft_geom::Rect::new(0, 0, 16, 12)));
        }
        d.layers.push(board);
        d.paths.push(photocraft_doc::NamedPath { name: "Path 1".into(), path: photocraft_vector::shapes::rect(1.0, 1.0, 5.0, 5.0), psd_raw: None });
        d.work_path = Some(photocraft_vector::shapes::ellipse(3.0, 3.0, 6.0, 4.0));
        d.notes.push(photocraft_doc::Note { text: "note".into(), author: "me".into(), ..Default::default() });
        v.push(("rich".into(), photocraft_io::document_to_psd(&d)));
    }
    v
}

fn testgen_imports() {
    for c in photocraft_psd::testgen::all_cases() {
        let bytes = bytes_of(&c.file);
        println!("tg\t{}\t{}", c.name, import(&bytes).0);
    }
}

fn synthetic(files: &[(String, PsdFile)]) {
    for (name, f) in files {
        let bytes = bytes_of(f);
        println!("imp\t{name}\t{}\t{}", b64(&bytes), import(&bytes).0);
    }
}

/// xorshift32 (the C test uses the same generator).
fn xs(s: &mut u32) -> u32 {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    *s
}

fn sweeps(files: &[(String, PsdFile)]) {
    for (name, f) in files.iter().filter(|(n, _)| matches!(n.as_str(), "raster" | "masks" | "options" | "effects" | "groups" | "adj-fill" | "text" | "smart" | "resources" | "alpha-rgb8" | "merged-indexed")) {
        let bytes = bytes_of(f);
        // Truncations: every length up to 512, then every 61st.
        let mut acc = String::new();
        let mut n = 0;
        let mut len = 0usize;
        while len < bytes.len() {
            acc.push_str(&import(&bytes[..len]).1);
            acc.push('\n');
            n += 1;
            len += if len < 512 { 1 } else { 61 };
        }
        println!("sweep\t{name}\ttrunc\t{n}\t{}", h16(acc.as_bytes()));
        // Mutations: 200 single-byte changes from a seeded generator.
        let mut acc = String::new();
        let mut s = 0x1234_5678u32 ^ bytes.len() as u32;
        for _ in 0..200 {
            let mut b = bytes.clone();
            let at = xs(&mut s) as usize % b.len();
            b[at] = (xs(&mut s) & 0xff) as u8;
            let r = import(&b);
            if std::env::var("PSDV_SWEEP").is_ok_and(|v| v == *name) {
                println!("sweepcase\t{at}\t{}", r.0);
            }
            acc.push_str(&r.1);
            acc.push('\n');
        }
        println!("sweep\t{name}\tmut\t200\t{}", h16(acc.as_bytes()));
    }
}

fn files_in(root: &std::path::Path, exts: &[&str]) -> Vec<std::path::PathBuf> {
    let mut out = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(d) = stack.pop() {
        let Ok(rd) = std::fs::read_dir(&d) else { continue };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                stack.push(p);
            } else if p.extension().and_then(|x| x.to_str()).is_some_and(|x| exts.contains(&x.to_ascii_lowercase().as_str())) {
                out.push(p);
            }
        }
    }
    out.sort();
    out
}

fn corpus() {
    let root = std::path::Path::new("corpus");
    for dir in ["psd", "psd-tools", "photoshop"] {
        for p in files_in(&root.join(dir), &["psd", "psb"]) {
            let Ok(bytes) = std::fs::read(&p) else { continue };
            let (full, hashed) = import(&bytes);
            if std::env::var_os("PSDV_FULL").is_some() {
                println!("corpfull\t{}\t{full}", p.display());
            }
            println!("corp\t{}\t{hashed}", p.display());
        }
    }
}

pub fn run() {
    std::panic::set_hook(Box::new(|_| {}));
    fills();
    colors();
    effects();
    blocks_misc();
    adjustments();
    testgen_imports();
    let files = synth_files();
    synthetic(&files);
    sweeps(&files);
    corpus();
    let _ = (pmode(ColorMode::Rgb), PixelFormat::RGBA8, BlendMode::Normal);
}
