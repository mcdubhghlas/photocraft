//! I4 vector/text/comps/slices/annotations/patterns/brushes/gradients maps reference vectors
//! (`rustref io-maps-vectors`), checked by tests/io/test_maps_parity.c. Lines are tab-separated;
//! the first field names the kind, the second the case. Model inputs are printed as serde JSON
//! (the C side reads them with the doc JSON readers); byte inputs and outputs as base64. Results
//! of decoders are `some …` / `none`. Truncation and mutation sweeps of every binary input are
//! named `<case>:cutN` / `<case>:mutN=V`.
//!
//!   vres CASE W H B64 RES          vector_map::path_from_resource           -> some PATHJSON | none
//!   vmsk CASE W H B64 RES          path_from_vmsk + vector_mask_from_block (+ its bytes, matches)
//!   vstk CASE DPI B64 RES          parse_vstk                               -> some STROKE FILLEN
//!   vscg CASE B64 RES              fill_from_vscg
//!   vogk CASE B64 RES              live_from_vogk
//!   vrec CASE W H PATH RES         path_to_records bytes, then path_from_records
//!   vmskw CASE W H FLAGS PATH RES  vmsk_bytes
//!   vmaskw CASE W H MASK RES       vector_mask_bytes
//!   vstkw CASE DPI FILLEN STROKE RES | vscgw CASE FILL RES | vogkw CASE DPI LIVE RES
//!   shape CASE SHAPE RAWIN RES     shape_blocks (blocks out) + shape_from_blocks of the result
//!   art CASE B64 RES | artw CASE ART GUIDES RES | gin CASE GUIDES RECT RES
//!   cdec CASE RES1065 CMLS RES     comps_from_psd for a one-layer document whose shmd holds CMLS
//!   cw CASE RES                    write_comps_resource / write_cmls / comps_unchanged (built docs)
//!   anno CASE B64 RES | annow CASE NOTES RES | anx CASE RES (export_blocks on built docs)
//!   scale CASE B64 RES | scalew CASE SCALE RES | scx CASE RES (keep/fresh rules)
//!   slc CASE B64 RES | slx CASE RES   slices_from_psd (layer ids as walk indices) / export_resource
//!   pblk CASE B64 RES | pat CASE B64 RES | ptx CASE RES   patterns (block, .pat, round trips)
//!   gb CASE IN RES                 gradient_bake::bake (copied in with #[path])
//!   abr CASE B64 RES | abrg CASE RES   abr_map::read_abr (embedded / generated inputs)
//!   ts CASE RES                    text_styles_map export_tysh + import on built type layers
//!   ti CASE RES                    text_import::prepare (copied in with #[path]; bundled fonts)
//!   corp PATH RES                  the maps over every block / resource of corpus PSDs
//!   corpi PATH RES                 psd_to_document of corpus PSDs: what the maps put in the model
use base64::Engine;
use photocraft_color::{Color, ColorMode, PixelFormat, SampleType};
use photocraft_doc::slices::{Slice, SliceKind, SliceOrigin, Slices};
use photocraft_doc::text::{CharStyle, ParagraphRun, ParagraphStyle, TextAlign, TextRun};
use photocraft_doc::text_styles::{CharacterStyleDef, ParagraphStyleDef};
use photocraft_doc::{
    Artboard, ArtboardBackground, CompLayerState, Document, Fill, FillRule, Guides, Knot, Layer, LayerComp, LayerContent, LiveShape, MeasurementScale,
    Note, Path, PathOp, Pattern, ShapeLayer, ShapeStroke, Size, Subpath, TextLayer, TextStyles, VectorMask,
};
use photocraft_geom::{Point, Rect};
use photocraft_io::{abr_map, annotations_map as an, comps_map as cm, pattern_map as pm, slices_map as sm, text_styles_map as tsm, vector_map as vm};
use photocraft_psd::descriptor::{Descriptor, Id, UnicodeString, Value, VersionedDescriptor};
use photocraft_psd::metadata::{MetadataItem, write_shmd};
use photocraft_psd::path::{PathData, PsdKnot, PsdSubpath, VectorMaskBlock};
use photocraft_psd::patterns::PsdPattern;
use photocraft_raster::Surface;

#[allow(dead_code)]
#[path = "../../../crates/io/src/gradient_bake.rs"]
mod gradient_bake;

/// crates/io/src/text_import.rs (private there, and written with let chains, which this 2021
/// crate can't compile): copied with the chains unrolled, behaviour unchanged.
mod text_import {
    use photocraft_doc::{Document, Layer, LayerContent, TextLayer};

    pub(crate) const MAX_RENDER_PIXELS: u64 = 64 * 1024 * 1024;

    fn render_bounded(eng: &mut photocraft_text::TextEngine, t: &mut TextLayer, dpi: f32, format: photocraft_color::PixelFormat) {
        let layout = eng.layout(t, dpi);
        let warp = photocraft_text::render::layout_warp(&layout, t.warp.as_ref());
        let ink = photocraft_text::render::ink_rect_warped(&layout, &t.transform, warp.as_ref());
        if u64::from(ink.width()).saturating_mul(u64::from(ink.height())) > MAX_RENDER_PIXELS {
            t.cache = Some(photocraft_raster::Surface::new(photocraft_color::PixelFormat { alpha: true, ..format }));
            return;
        }
        let r = photocraft_text::render::rasterize_warped(&layout, &t.transform, format, t.antialias, warp.as_ref());
        t.cache = Some(r.surface);
    }

    pub(crate) fn prepare(doc: &mut Document) {
        fn visit(layers: &mut [Layer], eng: &mut photocraft_text::TextEngine, dpi: f32, format: photocraft_color::PixelFormat) {
            for l in layers {
                match &mut l.content {
                    LayerContent::Text(t) => {
                        let mut changed = false;
                        for r in &mut t.runs {
                            if let Some(ps) = r.style.postscript_name.clone() {
                                if !eng.fonts.has_family(&r.style.font_family) {
                                    let f = eng.fonts.resolve_postscript(&ps);
                                    if f.exact || eng.fonts.has_family(&f.family) {
                                        r.style.font_family = f.family;
                                        r.style.weight = f.weight;
                                        r.style.italic = f.italic;
                                        changed = true;
                                    }
                                }
                            }
                        }
                        if changed {
                            t.sync_summary();
                        }
                        if t.cache.is_none() {
                            render_bounded(eng, t, dpi, format);
                        }
                    }
                    LayerContent::Group(g) => visit(&mut g.children, eng, dpi, format),
                    _ => {}
                }
            }
        }
        let dpi = doc.resolution_dpi;
        let format = doc.pixel_format();
        let mut eng = photocraft_text::shared().lock().unwrap_or_else(|e| e.into_inner());
        visit(&mut doc.layers, &mut eng, dpi, format);
    }
}

macro_rules! j {
    ($v:expr) => {
        serde_json::to_string(&$v).unwrap()
    };
}

fn b64(b: &[u8]) -> String {
    base64::engine::general_purpose::STANDARD.encode(b)
}
fn h16(b: &[u8]) -> String {
    blake3::hash(b).to_hex()[..16].to_string()
}
fn fb(v: f32) -> String {
    format!("{:08x}", v.to_bits())
}
fn opt_b64(o: Option<Vec<u8>>) -> String {
    o.map_or("none".into(), |b| format!("some {}", b64(&b)))
}

/// Truncations and byte mutations of an input (names are suffixes of the case).
fn sweeps(b: &[u8]) -> Vec<(String, Vec<u8>)> {
    let n = b.len();
    let mut cuts: Vec<usize> = if n <= 160 { (0..n).collect() } else { (0..64).chain((0..48).map(|i| 64 + i * (n - 80) / 48)).chain(n - 16..n).collect() };
    cuts.sort_unstable();
    cuts.dedup();
    let mut out: Vec<(String, Vec<u8>)> = cuts.into_iter().map(|c| (format!("cut{c}"), b[..c].to_vec())).collect();
    if n > 0 {
        let mut pos: Vec<usize> = (0..n.min(16)).chain((0..24).map(|i| i * n / 24)).collect();
        pos.sort_unstable();
        pos.dedup();
        for p in pos {
            for v in [0xFFu8, 0x00, b[p].wrapping_add(1)] {
                if v == b[p] {
                    continue;
                }
                let mut m = b.to_vec();
                m[p] = v;
                out.push((format!("mut{p}={v:02x}"), m));
            }
        }
    }
    out
}

// ---------------------------------------------------------------------------------------------
// vector_map
// ---------------------------------------------------------------------------------------------

fn sample_path() -> Path {
    let mut p = Path::new(vec![
        Subpath {
            closed: true,
            op: PathOp::Combine,
            knots: vec![Knot::corner(10.0, 10.0), Knot::smooth(Point::new(50.0, 20.0), Point::new(40.0, 10.0), Point::new(60.0, 30.0)), Knot::corner(20.0, 40.0)],
        },
        Subpath::polyline(&[(1.0, 2.0), (3.0, 4.0)]).with_op(PathOp::Subtract),
    ]);
    p.inverted = false;
    p
}

fn paths() -> Vec<(&'static str, Path)> {
    let mut v = vec![("sample", sample_path()), ("empty", Path::new(vec![]))];
    let mut inv = sample_path();
    inv.inverted = true;
    v.push(("inverted", inv));
    let mut emptyinv = Path::new(vec![]);
    emptyinv.inverted = true;
    v.push(("empty-inverted", emptyinv));
    let mut lead = sample_path();
    lead.subpaths[0].op = PathOp::Subtract;
    v.push(("lead-subtract", lead));
    let ops = [PathOp::Combine, PathOp::Subtract, PathOp::Intersect, PathOp::Exclude, PathOp::Join];
    let subs: Vec<Subpath> = ops.iter().enumerate().map(|(i, op)| Subpath::polygon(&[(i as f64, 0.5), (i as f64 + 7.25, 1.0), (i as f64 + 3.0, 9.75)]).with_op(*op)).collect();
    v.push(("all-ops", Path::new(subs)));
    let mut far = Path::new(vec![Subpath::polygon(&[(-40000.0, -1.0e9), (1.0e9, 3.3), (0.123456789, 99999.5)])]);
    far.fill_rule = FillRule::EvenOdd;
    v.push(("far", far));
    v.push(("open-one", Path::new(vec![Subpath { closed: false, op: PathOp::Exclude, knots: vec![Knot::corner(5.0, 6.0)] }])));
    v
}

fn psd_sub(ops: &[i16], closed: bool) -> Vec<PsdSubpath> {
    let square = |x: f64| PsdSubpath {
        closed,
        operation: 0,
        knots: [(x, 0.25), (x + 0.25, 0.25), (x + 0.25, 0.5), (x, 0.5)].iter().map(|&p| PsdKnot { linked: x > 0.15, pre: p, anchor: p, post: p }).collect(),
    };
    ops.iter().enumerate().map(|(i, &op)| PsdSubpath { operation: op, ..square(0.1 * i as f64) }).collect()
}

fn vmsk_inputs() -> Vec<(String, Vec<u8>)> {
    let mut v = Vec::new();
    for (name, init, ops) in [
        ("init-combine", true, &[1i16][..]),
        ("lead-subtract", true, &[2, 1, 2, 0][..]),
        ("join", false, &[1, 2, -1][..]),
        ("lead-join", false, &[2, -1, 1][..]),
        ("reveal", true, &[][..]),
        ("hide", false, &[][..]),
        ("odd-ops", false, &[7, -5, 3, 0x7fff][..]),
    ] {
        for flags in [0u32, 1, 2, 4, 7, 0xffff_fff8] {
            let b = VectorMaskBlock { version: 3, flags, path: PathData::from_subpaths(&psd_sub(ops, true), init) }.to_bytes();
            v.push((format!("{name}/f{flags}"), b));
        }
    }
    let open = VectorMaskBlock { version: 3, flags: 0, path: PathData::from_subpaths(&psd_sub(&[1, 2], false), false) }.to_bytes();
    v.push(("open".into(), open));
    // Records without length records (lenient reading) and a trailing partial record.
    let mut raw = 3u32.to_be_bytes().to_vec();
    raw.extend(0u32.to_be_bytes());
    let mut pd = PathData::from_subpaths(&psd_sub(&[1], true), false).to_bytes();
    pd.drain(2 * 26..3 * 26);
    raw.extend(pd);
    raw.extend([0, 1, 2]);
    v.push(("no-length".into(), raw));
    v
}

fn path_res(p: Option<Path>) -> String {
    p.map_or("none".into(), |p| format!("some {}", j!(p)))
}

fn vmsk_res(data: &[u8], w: u32, h: u32) -> String {
    let a = vm::path_from_vmsk(data, w, h).map_or("none".into(), |(p, f)| format!("some {} {f}", j!(p)));
    let m = match vm::vector_mask_from_block(data, w, h) {
        Some(m) => format!("some {} {} {}", j!(m), b64(&vm::vector_mask_bytes(&m, w, h)), vm::vector_mask_block_matches(data, &m, w, h)),
        None => "none".into(),
    };
    format!("{a} | {m}")
}

fn vstk_res(d: &[u8], dpi: f32) -> String {
    vm::parse_vstk(d, dpi).map_or("none".into(), |v| format!("some {} {}", v.stroke.as_ref().map_or("null".into(), |s| j!(s)), v.fill_enabled))
}

fn vscg_res(d: &[u8]) -> String {
    vm::fill_from_vscg(d).map_or("none".into(), |f| format!("some {}", j!(f)))
}

fn vogk_res(d: &[u8]) -> String {
    vm::live_from_vogk(d).map_or("none".into(), |l| format!("some {}", j!(l)))
}

fn strokes() -> Vec<(&'static str, ShapeStroke)> {
    let red = ShapeStroke {
        width: 4.5,
        paint: Fill::Solid(Color::rgb(1.0, 0.0, 0.0)),
        opacity: 0.5,
        align: photocraft_doc::StrokeAlign::Outside,
        cap: photocraft_doc::LineCap::Round,
        join: photocraft_doc::LineJoin::Bevel,
        miter_limit: 10.0,
        dashes: vec![2.0, 1.0],
        dash_offset: 0.5,
    };
    let grad = ShapeStroke {
        width: 0.25,
        paint: gradient_fill(),
        align: photocraft_doc::StrokeAlign::Inside,
        cap: photocraft_doc::LineCap::Square,
        join: photocraft_doc::LineJoin::Round,
        ..Default::default()
    };
    let pat = ShapeStroke { paint: pattern_fill(), dashes: vec![0.0, 3.5, 1.25], opacity: 0.3333, ..Default::default() };
    vec![("default", ShapeStroke::default()), ("red", red), ("gradient", grad), ("pattern", pat)]
}

fn gradient_fill() -> Fill {
    Fill::Gradient {
        stops: vec![(0.0, Color::rgb(1.0, 0.0, 0.0)), (0.4, Color::rgb(0.0, 1.0, 0.5)), (1.0, Color::rgb(0.0, 0.0, 1.0))],
        angle: 30.0,
        scale: 1.0,
        style: photocraft_doc::effects::GradientStyle::Radial,
        reverse: true,
        opacity_stops: vec![],
        midpoints: vec![],
        offset: (0.0, 0.0),
        dither: false,
        align: true,
    }
}

fn pattern_fill() -> Fill {
    Fill::Pattern { name: "Grit".into(), scale: 0.5, id: "b6c4e4b8-0000".into(), angle: 15.0, link: true, phase: (0.0, 0.0) }
}

fn fills() -> Vec<(&'static str, Fill)> {
    vec![
        ("solid", Fill::Solid(Color::rgb(0.0, 1.0, 0.0))),
        ("cmyk", Fill::Solid(Color { mode: ColorMode::Cmyk, c: [0.2, 0.4, 0.6, 0.1], alpha: 1.0 })),
        ("gradient", gradient_fill()),
        ("pattern", pattern_fill()),
    ]
}

fn lives() -> Vec<(&'static str, LiveShape)> {
    vec![
        ("rect", LiveShape::Rect { rect: [1.0, 2.0, 30.0, 40.0], radii: [0.0; 4] }),
        ("rrect", LiveShape::Rect { rect: [1.0, 2.0, 30.0, 40.0], radii: [1.0, 2.0, 3.0, 4.0] }),
        ("ellipse", LiveShape::Ellipse { rect: [5.0, 6.0, 7.0, 8.0] }),
        ("line", LiveShape::Line { from: [0.0, 10.0], to: [20.0, 0.0], weight: 3.0 }),
        ("line-neg", LiveShape::Line { from: [-0.0, 10.5], to: [0.0, -3.25], weight: 0.5 }),
        ("polygon", LiveShape::Polygon { rect: [0.0; 4], sides: 5, star_ratio: 1.0 }),
    ]
}

/// A vogk block with a hand-made origination (for the decoder's rejection rules).
fn vogk_custom(o: Descriptor, list_extra: bool) -> Vec<u8> {
    let mut list = vec![Value::Descriptor(o)];
    if list_extra {
        list.push(Value::Integer(1));
    }
    let d = Descriptor::new("null").with("keyDescriptorList", Value::List(list));
    let mut out = 1u32.to_be_bytes().to_vec();
    out.extend(VersionedDescriptor::new(d).to_bytes());
    out
}

fn bbox(t: f64, l: f64, b: f64, r: f64) -> Value {
    Value::Descriptor(Descriptor::new("unitRect").with("Top ", Value::Double(t)).with("Left", Value::Integer(l as i32)).with("Btom", Value::UnitFloat { unit: *b"#Pxl", value: b }).with("Rght", Value::Double(r)))
}

fn vogk_inputs() -> Vec<(String, Vec<u8>)> {
    let mut v: Vec<(String, Vec<u8>)> = lives().into_iter().filter_map(|(n, l)| vm::vogk_bytes(&l, 72.0).map(|b| (n.to_string(), b))).collect();
    let base = |ty: i32| Descriptor::new("null").with("keyOriginType", Value::Integer(ty)).with("keyOriginShapeBBox", bbox(1.0, 2.0, 30.5, 40.0));
    v.push(("invalidated".into(), vogk_custom(base(1).with("keyShapeInvalidated", Value::Boolean(true)), false)));
    v.push(("transformed".into(), vogk_custom(base(5).with("Trnf", Value::Descriptor(Descriptor::new("Trnf").with("xx", Value::Double(1.0)).with("tx", Value::Double(3.0)))), false)));
    v.push(("ident-trnf".into(), vogk_custom(base(5).with("Trnf", Value::Descriptor(Descriptor::new("Trnf").with("xx", Value::Double(1.0)).with("yy", Value::Integer(1)))), false)));
    v.push(("two".into(), vogk_custom(base(1), true)));
    v.push(("type-double".into(), vogk_custom(Descriptor::new("null").with("keyOriginType", Value::Double(1.0)).with("keyOriginShapeBBox", bbox(1.0, 2.0, 3.0, 4.0)), false)));
    v.push(("type-7".into(), vogk_custom(base(7), false)));
    v.push(("no-bbox".into(), vogk_custom(Descriptor::new("null").with("keyOriginType", Value::Integer(1)), false)));
    v.push(("rrect-no-radii".into(), vogk_custom(base(2), false)));
    v.push(("line-no-weight".into(), vogk_custom(Descriptor::new("null").with("keyOriginType", Value::Integer(4)).with("keyOriginLineStart", Value::Descriptor(Descriptor::new("Pnt ").with("Hrzn", Value::Integer(3)).with("Vrtc", Value::Double(4.5)))).with("keyOriginLineEnd", Value::Descriptor(Descriptor::new("Pnt ").with("Hrzn", Value::Double(-1.0)).with("Vrtc", Value::Double(2.0)))), false)));
    v
}

fn vstk_inputs() -> Vec<(String, Vec<u8>)> {
    let mut v = Vec::new();
    for (n, s) in strokes() {
        v.push((format!("{n}/on"), vm::vstk_bytes(Some(&s), true, 72.0)));
        v.push((format!("{n}/nofill"), vm::vstk_bytes(Some(&s), false, 300.0)));
    }
    v.push(("off".into(), vm::vstk_bytes(None, true, 72.0)));
    v.push(("off-nofill".into(), vm::vstk_bytes(None, false, 72.0)));
    // Units, a content class to infer, and missing fields.
    let d = Descriptor::new("strokeStyle")
        .with("strokeEnabled", Value::Boolean(true))
        .with("strokeStyleLineWidth", Value::UnitFloat { unit: *b"#Pnt", value: 3.0 })
        .with("strokeStyleContent", Value::Descriptor(Descriptor::new("other").with("Clr ", Value::Descriptor(Descriptor::new("RGBC").with("Rd  ", Value::Double(255.0)).with("Grn ", Value::Double(0.0)).with("Bl  ", Value::Double(0.0))))));
    v.push(("points".into(), VersionedDescriptor::new(d).to_bytes()));
    let d = Descriptor::new("strokeStyle").with("strokeEnabled", Value::Boolean(true)).with("strokeStyleLineWidth", Value::UnitFloat { unit: *b"#Mlm", value: 1.0 }).with("strokeStyleLineDashSet", Value::List(vec![Value::Integer(2), Value::Text(UnicodeString::new("x")), Value::Double(0.5)]));
    v.push(("mm".into(), VersionedDescriptor::new(d).to_bytes()));
    let d = Descriptor::new("strokeStyle").with("strokeEnabled", Value::Integer(1)).with("fillEnabled", Value::Integer(0));
    v.push(("int-flags".into(), VersionedDescriptor::new(d).to_bytes()));
    v
}

fn shape_cases() -> Vec<(String, ShapeLayer, Vec<([u8; 4], Vec<u8>)>)> {
    let base = ShapeLayer {
        path: sample_path(),
        fill: Some(Fill::Solid(Color::rgb(0.0, 1.0, 0.0))),
        stroke: Some(ShapeStroke::default()),
        live: Some(LiveShape::Ellipse { rect: [0.0, 0.0, 10.0, 10.0] }),
        ..Default::default()
    };
    let mut v = Vec::new();
    v.push(("fresh".to_string(), base.clone(), Vec::new()));
    let mut raw = Vec::new();
    vm::shape_blocks(&base, &mut raw, 64, 64, 72.0);
    v.push(("unchanged".to_string(), base.clone(), raw.clone()));
    let mut edited = base.clone();
    edited.path.subpaths.pop();
    edited.live = Some(LiveShape::Polygon { rect: [0.0; 4], sides: 3, star_ratio: 1.0 });
    v.push(("edited".to_string(), edited, raw.clone()));
    let mut nofill = base.clone();
    nofill.fill = None;
    v.push(("nofill".to_string(), nofill.clone(), raw.clone()));
    v.push(("nofill-fresh".to_string(), nofill, Vec::new()));
    let mut nostroke = base.clone();
    nostroke.stroke = None;
    nostroke.live = None;
    v.push(("nostroke".to_string(), nostroke.clone(), raw.clone()));
    v.push(("nostroke-fresh".to_string(), nostroke, Vec::new()));
    let mut grad = base.clone();
    grad.fill = Some(gradient_fill());
    v.push(("gradient".to_string(), grad, raw.clone()));
    // A vsms block and a separate SoCo fill block (older files), the psd_raw copy still valid.
    let vsms = vm::vector_mask_bytes(&VectorMask::new(base.path.clone()), 64, 64);
    let soco = vec![0u8, 0, 0, 16, 0, 0, 0, 0];
    let mut withraw = base.clone();
    withraw.psd_raw = Some(std::sync::Arc::new(vm::vmsk_bytes(&base.path, 2, 64, 64)));
    let mut changed = withraw.clone();
    changed.path.subpaths.truncate(1);
    v.push(("vsms".to_string(), withraw.clone(), vec![(*b"vsms", vsms.clone()), (*b"SoCo", soco.clone()), (*b"luni", vec![0, 0, 0, 1, 0, 65])]));
    v.push(("psd-raw".to_string(), withraw, vec![(*b"vmsk", vec![1, 2, 3])]));
    v.push(("psd-raw-stale".to_string(), changed, vec![(*b"vmsk", vec![1, 2, 3])]));
    let mut undecodable = base.clone();
    undecodable.fill = None;
    v.push(("bad-fill".to_string(), undecodable.clone(), vec![(*b"vscg", b"SoCo\0\0\0\x10junk".to_vec()), (*b"vstk", vm::vstk_bytes(Some(&ShapeStroke::default()), true, 72.0))]));
    v.push(("bad-fill-off".to_string(), undecodable, vec![(*b"vscg", b"SoCo\0\0\0\x10junk".to_vec()), (*b"vstk", vm::vstk_bytes(Some(&ShapeStroke::default()), false, 72.0))]));
    let mut live_rect = base.clone();
    live_rect.live = Some(LiveShape::Rect { rect: [3.0, 4.0, 5.0, 6.0], radii: [1.0, 0.0, 0.0, 0.0] });
    v.push(("live-changed".to_string(), live_rect, raw.clone()));
    let mut no_live = base.clone();
    no_live.live = None;
    v.push(("live-removed".to_string(), no_live, raw));
    v
}

fn shape_json(sh: &ShapeLayer) -> String {
    format!(
        "{{\"path\":{},\"fill\":{},\"stroke\":{},\"live\":{},\"psd_raw\":{}}}",
        j!(sh.path),
        j!(sh.fill),
        j!(sh.stroke),
        j!(sh.live),
        sh.psd_raw.as_ref().map_or("null".into(), |r| format!("\"{}\"", b64(r)))
    )
}

fn raw_s(raw: &[([u8; 4], Vec<u8>)]) -> String {
    if raw.is_empty() {
        return "-".into();
    }
    raw.iter().map(|(k, d)| format!("{}:{}", String::from_utf8_lossy(k), b64(d))).collect::<Vec<_>>().join(",")
}

fn vector() {
    for (name, p) in paths() {
        for (w, h) in [(64u32, 64u32), (100, 37), (0, 0)] {
            let rec = vm::path_to_records(&p, w, h);
            let back = vm::path_from_records(&rec, w, h);
            println!("vrec\t{name}\t{w}\t{h}\t{}\t{} {}", j!(p), b64(&rec.to_bytes()), j!(back));
            for flags in [0u32, 1, 2, 4, 6] {
                println!("vmskw\t{name}\t{w}\t{h}\t{flags}\t{}\t{}", j!(p), b64(&vm::vmsk_bytes(&p, flags, w, h)));
            }
        }
        for (en, ln) in [(true, true), (false, false), (true, false)] {
            let mut m = VectorMask::new(p.clone());
            m.enabled = en;
            m.linked = ln;
            m.density = 0.5;
            println!("vmaskw\t{name}/{en}{ln}\t64\t48\t{}\t{}", j!(m), b64(&vm::vector_mask_bytes(&m, 64, 48)));
        }
    }
    for (name, b) in vmsk_inputs() {
        for (w, h) in [(100u32, 100u32), (64, 7)] {
            println!("vmsk\t{name}\t{w}\t{h}\t{}\t{}", b64(&b), vmsk_res(&b, w, h));
        }
        let pd = PathData::from_subpaths(&psd_sub(&[1, 2], true), true).to_bytes();
        let _ = pd;
    }
    if let Some((_, b)) = vmsk_inputs().into_iter().find(|(n, _)| n == "join/f2") {
        for (s, m) in sweeps(&b) {
            println!("vmsk\tjoin:{s}\t100\t100\t{}\t{}", b64(&m), vmsk_res(&m, 100, 100));
        }
    }
    for (name, p) in paths() {
        let b = vm::path_to_records(&p, 50, 50).to_bytes();
        println!("vres\t{name}\t50\t50\t{}\t{}", b64(&b), path_res(vm::path_from_resource(&b, 50, 50)));
        if name == "sample" {
            for (s, m) in sweeps(&b) {
                println!("vres\t{name}:{s}\t50\t50\t{}\t{}", b64(&m), path_res(vm::path_from_resource(&m, 50, 50)));
            }
        }
    }
    for (name, s) in strokes() {
        for (dpi, fe) in [(72.0f32, true), (300.0, false)] {
            println!("vstkw\t{name}\t{}\t{fe}\t{}\t{}", fb(dpi), j!(s), b64(&vm::vstk_bytes(Some(&s), fe, dpi)));
        }
    }
    println!("vstkw\tnone\t{}\ttrue\tnull\t{}", fb(72.0), b64(&vm::vstk_bytes(None, true, 72.0)));
    for (name, b) in vstk_inputs() {
        for dpi in [72.0f32, 150.0] {
            println!("vstk\t{name}\t{}\t{}\t{}", fb(dpi), b64(&b), vstk_res(&b, dpi));
        }
        if name == "red/on" {
            for (s, m) in sweeps(&b) {
                println!("vstk\t{name}:{s}\t{}\t{}\t{}", fb(72.0), b64(&m), vstk_res(&m, 72.0));
            }
        }
    }
    for (name, f) in fills() {
        let b = vm::vscg_bytes(&f);
        println!("vscgw\t{name}\t{}\t{}", j!(f), b64(&b));
        println!("vscg\t{name}\t{}\t{}", b64(&b), vscg_res(&b));
        if name == "solid" {
            for (s, m) in sweeps(&b) {
                println!("vscg\t{name}:{s}\t{}\t{}", b64(&m), vscg_res(&m));
            }
        }
    }
    for (name, l) in lives() {
        for dpi in [72.0f32, 96.5] {
            println!("vogkw\t{name}\t{}\t{}\t{}", fb(dpi), j!(l), opt_b64(vm::vogk_bytes(&l, dpi)));
        }
    }
    for (name, b) in vogk_inputs() {
        println!("vogk\t{name}\t{}\t{}", b64(&b), vogk_res(&b));
        if name == "rrect" || name == "line" {
            for (s, m) in sweeps(&b) {
                println!("vogk\t{name}:{s}\t{}\t{}", b64(&m), vogk_res(&m));
            }
        }
    }
    for (name, sh, raw) in shape_cases() {
        let mut out = raw.clone();
        vm::shape_blocks(&sh, &mut out, 64, 64, 72.0);
        let mut back = ShapeLayer::default();
        let lookup = |k: &[u8; 4]| out.iter().find(|(key, _)| key == k).map(|(_, d)| d.clone());
        vm::shape_from_blocks(&mut back, &lookup, 64, 64, 72.0);
        println!(
            "shape\t{name}\t{}\t{}\t{} | {} {} {} {}",
            shape_json(&sh),
            raw_s(&raw),
            raw_s(&out),
            j!(back.path),
            j!(back.fill),
            j!(back.stroke),
            j!(back.live)
        );
    }
}

// ---------------------------------------------------------------------------------------------
// comps_map
// ---------------------------------------------------------------------------------------------

fn art_res(d: &[u8]) -> String {
    cm::parse_artboard(d).map_or("none".into(), |a| format!("some {}", j!(a)))
}

fn artboards() -> Vec<(&'static str, Artboard)> {
    vec![
        ("custom", Artboard { rect: Rect::new(10, 20, 110, 220), background: ArtboardBackground::Custom(Color::rgb(0.2, 0.4, 0.6)), preset: "Web".into() }),
        ("white", Artboard::new(Rect::new(0, 0, 4, 4))),
        ("black", Artboard { background: ArtboardBackground::Black, ..Artboard::new(Rect::new(-5, -6, 7, 8)) }),
        ("transparent", Artboard { background: ArtboardBackground::Transparent, preset: "iPhone 8/7/6 \u{e9}\u{1f600}".into(), ..Artboard::new(Rect::new(0, 0, 375, 667)) }),
    ]
}

fn comp_doc(cmls: Option<&[u8]>) -> (Document, photocraft_doc::LayerId) {
    let mut d = Document::with_background("t", Size::new(32, 32), ColorMode::Rgb, SampleType::U8, Color::WHITE);
    let mut l = Layer::raster("A", PixelFormat::RGBA8);
    l.surface_mut().unwrap().fill_rect(Rect::new(4, 4, 8, 8), &[1.0, 0.0, 0.0, 1.0]);
    if let Some(c) = cmls {
        l.psd_blocks = vec![(*b"shmd", std::sync::Arc::new(write_shmd(&[MetadataItem::new(*b"cust", vec![1, 2]), MetadataItem::new(*b"cmls", c.to_vec())])))];
    }
    let lid = l.id;
    d.layers.push(l);
    (d, lid)
}

/// Comps with layer ids replaced by the layer's index in the document walk.
fn comps_s(doc: &Document, comps: &[LayerComp], last_applied: Option<u32>, last: Option<&LayerComp>) -> String {
    let ids: Vec<photocraft_doc::LayerId> = doc.walk().into_iter().map(|(_, _, l)| l.id).collect();
    let one = |c: &LayerComp| {
        let states: Vec<String> = c
            .states
            .iter()
            .map(|s| {
                let ix = ids.iter().position(|i| *i == s.layer).map_or("?".to_string(), |i| i.to_string());
                let vis = s.visible.map_or("-".into(), |v| v.to_string());
                let pos = s.position.map_or("-".into(), |(x, y)| format!("{x},{y}"));
                format!("{ix}:{vis}:{pos}")
            })
            .collect();
        format!("{}|{:?}|{:?}|{}{}{}|[{}]", c.id, c.name, c.comment, u8::from(c.apply_visibility), u8::from(c.apply_position), u8::from(c.apply_appearance), states.join(";"))
    };
    format!(
        "C[{}] la={} last={}",
        comps.iter().map(one).collect::<Vec<_>>().join(" "),
        last_applied.map_or("-".into(), |v| v.to_string()),
        last.map_or("-".into(), one)
    )
}

fn cmls_desc(settings: Vec<Value>) -> Vec<u8> {
    VersionedDescriptor::new(Descriptor::new("null").with("layerSettings", Value::List(settings))).to_bytes()
}

fn nul() -> UnicodeString {
    UnicodeString(vec![0])
}

fn setting(enab: Option<Value>, ids: Vec<Value>, ofst: Option<(Value, Value)>) -> Value {
    let mut s = Descriptor { name: nul(), ..Descriptor::new("null") };
    if let Some(e) = enab {
        s = s.with("enab", e);
    }
    s = s.with("compList", Value::List(ids));
    if let Some((h, v)) = ofst {
        s = s.with("Ofst", Value::Descriptor(Descriptor { name: nul(), ..Descriptor::new("null") }.with("Hrzn", h).with("Vrtc", v)));
    }
    Value::Descriptor(s)
}

fn comps() {
    for (name, a) in artboards() {
        for guides in [&[][..], &[0, 2, -1][..]] {
            let b = cm::write_artboard(&a, guides);
            println!("artw\t{name}/{}\t{}\t{}\t{}", guides.len(), j!(a), j!(guides), b64(&b));
            println!("art\t{name}/{}\t{}\t{}", guides.len(), b64(&b), art_res(&b));
        }
    }
    let b = cm::write_artboard(&artboards()[0].1, &[1]);
    for (s, m) in sweeps(&b) {
        println!("art\tcustom:{s}\t{}\t{}", b64(&m), art_res(&m));
    }
    let older = VersionedDescriptor::new(
        Descriptor::new("artboard")
            .with("artboardRect", Value::GlobalObject(Descriptor::new("classFloatRect").with("Top ", Value::Integer(2)).with("Left", Value::UnitFloat { unit: *b"#Pxl", value: 1.5 }).with("Btom", Value::Double(-2.5)).with("Rght", Value::Double(1e12))))
            .with("artboardBackgroundType", Value::Double(2.0)),
    )
    .to_bytes();
    println!("art\tolder\t{}\t{}", b64(&older), art_res(&older));
    let mut g = Guides::default();
    g.vertical = vec![0.0, 5.0, 10.5, 110.0, 50.0];
    g.horizontal = vec![20.0, 21.0, 219.9, 220.0, -3.0];
    for (n, r) in [("in", Rect::new(10, 20, 110, 220)), ("empty", Rect::new(0, 0, 0, 0)), ("neg", Rect::new(-10, -10, 1, 1))] {
        println!("gin\t{n}\t{}\t{},{},{},{}\t{}", j!(g), r.x0, r.y0, r.x1, r.y1, j!(cm::guides_in(&g, r)));
    }
    // Decoding comps against a one-layer document at (4, 4).
    let comp = |id: Value, name: &str| {
        let mut c = Descriptor::new("Comp").with("Nm  ", Value::Text(UnicodeString::new_nul(name))).with("compID", id).with("capturedInfo", Value::Integer(3));
        if name == "B" {
            c = c.with("comment", Value::Text(UnicodeString::new_nul("note \u{e9}")));
        }
        Value::Descriptor(c)
    };
    let res = VersionedDescriptor::new(
        Descriptor::new("CompList")
            .with("list", Value::List(vec![comp(Value::Integer(1), "A"), comp(Value::Double(7.4), "B"), Value::Integer(5), comp(Value::LargeInteger(9), "C"), comp(Value::Text(nul()), "D")]))
            .with("lastAppliedComp", Value::Integer(7)),
    )
    .to_bytes();
    let cmls = cmls_desc(vec![
        setting(Some(Value::Boolean(true)), vec![Value::Integer(1), Value::Integer(0)], Some((Value::Integer(6), Value::Integer(-3)))),
        setting(Some(Value::Boolean(false)), vec![Value::Integer(7), Value::Double(9.0), Value::Integer(9)], None),
        setting(None, vec![Value::Integer(9)], Some((Value::Integer(i32::MAX), Value::Integer(0)))),
        setting(None, vec![Value::Integer(1)], None),
        Value::Integer(3),
        setting(Some(Value::Boolean(true)), vec![Value::Integer(-1)], Some((Value::LargeInteger(i64::from(u32::MAX) + 2), Value::Double(2.6)))),
    ]);
    let cdec = |name: &str, res: &[u8], cmls: &[u8]| {
        let (d, _) = comp_doc(if cmls.is_empty() { None } else { Some(cmls) });
        let r = if res.is_empty() { None } else { Some(res) };
        let (c, la, last) = cm::comps_from_psd(r, &d);
        println!("cdec\t{name}\t{}\t{}\t{}", if res.is_empty() { "-".into() } else { b64(res) }, if cmls.is_empty() { "-".into() } else { b64(cmls) }, comps_s(&d, &c, la, last.as_ref()));
    };
    cdec("full", &res, &cmls);
    cdec("no-res", &[], &cmls);
    cdec("no-cmls", &res, &[]);
    for (s, m) in sweeps(&res) {
        cdec(&format!("res:{s}"), &m, &cmls);
    }
    for (s, m) in sweeps(&cmls) {
        cdec(&format!("cmls:{s}"), &res, &m);
    }
    // Writing comps for built documents.
    let (mut d, lid) = comp_doc(None);
    d.layer_comps.push(LayerComp { id: 7, name: "One".into(), comment: "c".into(), apply_visibility: true, apply_position: true, apply_appearance: false, states: vec![CompLayerState { layer: lid, visible: Some(false), position: Some((10, 1)), appearance: None }] });
    d.layer_comps.push(LayerComp { id: u32::MAX, name: "Two \u{1f600}".into(), comment: String::new(), apply_visibility: false, apply_position: true, apply_appearance: true, states: vec![CompLayerState { layer: lid, visible: None, position: Some((i32::MIN, 0)), appearance: None }] });
    d.last_applied_comp = Some(7);
    let last = LayerComp { id: 0, name: "Last Document State".into(), comment: String::new(), apply_visibility: true, apply_position: true, apply_appearance: false, states: vec![CompLayerState { layer: lid, visible: Some(true), position: Some((4, 4)), appearance: None }] };
    let resb = cm::write_comps_resource(&d).unwrap();
    let l = d.layer(lid).unwrap().clone();
    let c1 = cm::write_cmls(&d.layer_comps, None, &l, 3).unwrap();
    let c2 = cm::write_cmls(&d.layer_comps, Some(&last), &l, u32::MAX).unwrap();
    println!("cw\tres\t{}", b64(&resb));
    println!("cw\tcmls\t{}", b64(&c1));
    println!("cw\tcmls-last\t{}", b64(&c2));
    println!("cw\tempty\t{}", opt_b64(cm::write_comps_resource(&comp_doc(None).0)));
    println!("cw\tempty-cmls\t{}", opt_b64(cm::write_cmls(&[], None, &l, 1)));
    // Attach and check comps_unchanged.
    let (mut d2, lid2) = comp_doc(Some(&c1));
    for c in &mut d.layer_comps {
        for s in &mut c.states {
            s.layer = lid2;
        }
    }
    d2.layer_comps = d.layer_comps.clone();
    d2.last_applied_comp = d.last_applied_comp;
    d2.metadata.psd_resources.push((cm::LAYER_COMPS, String::new(), std::sync::Arc::new(resb.clone())));
    let (c, la, lst) = cm::comps_from_psd(Some(&resb), &d2);
    println!("cw\tdecode\t{}", comps_s(&d2, &c, la, lst.as_ref()));
    println!("cw\tunchanged\t{}", cm::comps_unchanged(&d2));
    d2.layer_comps[0].name = "Renamed".into();
    println!("cw\tunchanged-renamed\t{}", cm::comps_unchanged(&d2));
}

// ---------------------------------------------------------------------------------------------
// annotations_map
// ---------------------------------------------------------------------------------------------

fn notes() -> Vec<Note> {
    vec![
        Note { text: "h\u{e9}llo \u{2713}\nline\r\nthree".into(), author: "me".into(), position: [3.0, 4.0], ..Default::default() },
        Note { author: "\u{1f600}x\u{e9}".into(), color: Color::rgb(0.25, 0.5, 1.0), text: String::new(), position: [-2.5, 1e12], popup: [1.0, 2.0, 3.0, 4.0], open: true, modified: "D:20210521231029+01'00'".into() },
        Note { color: Color { mode: ColorMode::Cmyk, c: [0.1, 0.2, 0.3, 0.4], alpha: 1.0 }, text: "\u{0}\u{0}tail\u{0}".into(), position: [f64::from(i32::MAX), -0.5], ..Default::default() },
    ]
}

fn anno_res(d: &[u8]) -> String {
    an::parse_anno(d).map_or("none".into(), |n| format!("some {}", j!(n)))
}

/// A sound entry (kept verbatim) and colour spaces the writer doesn't use.
fn anno_extra() -> Vec<u8> {
    let mut b = an::write_anno(&notes()[..1], &[]);
    // count = 4
    b[4..8].copy_from_slice(&4u32.to_be_bytes());
    let mut sound = Vec::new();
    sound.extend(20u32.to_be_bytes());
    sound.extend(b"sndA");
    sound.extend([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
    b.extend(&sound);
    for (space, c) in [(1u16, [61000u16, 32000, 65535, 0]), (8, [2500, 0, 0, 0])] {
        let mut n = an::write_anno(&notes()[1..2], &[])[8..].to_vec();
        // colour space at offset 4 (len) + 4 (type) + 4 (flags) + 32 (rects)
        let at = 4 + 4 + 4 + 32;
        n[at..at + 2].copy_from_slice(&space.to_be_bytes());
        for (i, v) in c.iter().enumerate() {
            n[at + 2 + 2 * i..at + 4 + 2 * i].copy_from_slice(&v.to_be_bytes());
        }
        b.extend(n);
    }
    b
}

fn ndoc() -> Document {
    Document::new("n", Size::new(10, 10), ColorMode::Rgb, SampleType::U8)
}

fn blocks_s(blocks: &[photocraft_doc::PsdGlobalBlock], orig: &[photocraft_doc::PsdGlobalBlock]) -> String {
    blocks
        .iter()
        .map(|(s, k, d)| {
            let same = orig.iter().any(|(_, _, o)| std::sync::Arc::ptr_eq(o, d));
            format!("{}{}:{}{}", String::from_utf8_lossy(s), String::from_utf8_lossy(k), b64(d), if same { "=" } else { "" })
        })
        .collect::<Vec<_>>()
        .join(",")
}

fn annotations() {
    let all = notes();
    for k in 0..=all.len() {
        let b = an::write_anno(&all[..k], &[]);
        println!("annow\tn{k}\t{}\t{}", j!(all[..k]), b64(&b));
        println!("anno\tn{k}\t{}\t{}", b64(&b), anno_res(&b));
    }
    let extra = anno_extra();
    println!("anno\textra\t{}\t{}", b64(&extra), anno_res(&extra));
    for (s, m) in sweeps(&an::write_anno(&all[..2], &[])) {
        println!("anno\tn2:{s}\t{}\t{}", b64(&m), anno_res(&m));
    }
    for (s, m) in sweeps(&extra) {
        println!("anno\textra:{s}\t{}\t{}", b64(&m), anno_res(&m));
    }
    let bad = vec![0u8, 2, 0, 1, 0, 0, 0, 9];
    println!("anno\tbad\t{}\t{}", b64(&bad), anno_res(&bad));
    // Export: keep, replace, drop, append; unreadable kept.
    let other: photocraft_doc::PsdGlobalBlock = (*b"8BIM", *b"Txt2", std::sync::Arc::new(vec![1, 2]));
    let anno_blk = |d: Vec<u8>| -> photocraft_doc::PsdGlobalBlock { (*b"8BIM", an::ANNO, std::sync::Arc::new(d)) };
    let cases: Vec<(&str, Vec<Note>, Vec<photocraft_doc::PsdGlobalBlock>)> = vec![
        ("none", vec![], vec![other.clone()]),
        ("new", all.clone(), vec![other.clone()]),
        ("same", all[..2].to_vec(), vec![anno_blk(an::write_anno(&all[..2], &[])), other.clone()]),
        ("changed", all[..1].to_vec(), vec![other.clone(), anno_blk(an::write_anno(&all[..2], &[]))]),
        ("deleted", vec![], vec![anno_blk(an::write_anno(&all[..2], &[])), other.clone()]),
        ("sound-kept", vec![], vec![anno_blk(extra.clone())]),
        ("sound-new", all[2..].to_vec(), vec![anno_blk(extra.clone())]),
        ("bad-kept", vec![], vec![anno_blk(bad.clone())]),
        ("bad-new", all[..1].to_vec(), vec![anno_blk(bad.clone()), other.clone()]),
    ];
    for (name, ns, blocks) in cases {
        let mut d = ndoc();
        d.notes = ns;
        d.metadata.psd_global_blocks = blocks.clone();
        let out = an::export_blocks(&d, blocks.clone());
        println!("anx\t{name}\t{}\t{}\t{}\t{}", j!(d.notes), blocks_s(&blocks, &[]), an::anno_unreadable(&d), blocks_s(&out, &blocks));
    }
    // Measurement scale.
    let scales = vec![
        MeasurementScale { pixel_length: 150.0, logical_length: 2.0, units: "mm".into() },
        MeasurementScale::default(),
        MeasurementScale { pixel_length: 0.1, logical_length: -3.0, units: "\u{e9}t\u{e9}s".into() },
    ];
    for (i, s) in scales.iter().enumerate() {
        let b = an::write_scale_resource(s);
        println!("scalew\ts{i}\t{}\t{}", j!(s), b64(&b));
        println!("scale\ts{i}\t{}\t{}", b64(&b), an::scale_from_resource(&b).map_or("none".into(), |s| format!("some {}", j!(s))));
        if i == 0 {
            for (c, m) in sweeps(&b) {
                println!("scale\ts{i}:{c}\t{}\t{}", b64(&m), an::scale_from_resource(&m).map_or("none".into(), |s| format!("some {}", j!(s))));
            }
        }
    }
    let nested = VersionedDescriptor::new(Descriptor::new("null").with("other", Value::Integer(1)).with("measurementScale", Value::Descriptor(Descriptor::new("null").with("PxlL", Value::LargeInteger(7)).with("LglL", Value::UnitFloat { unit: *b"#Pxl", value: 2.5 })))).to_bytes();
    println!("scale\tnested\t{}\t{}", b64(&nested), an::scale_from_resource(&nested).map_or("none".into(), |s| format!("some {}", j!(s))));
    for (name, raw, scale, counts) in [
        ("none-default", None, MeasurementScale::default(), false),
        ("none-set", None, scales[0].clone(), false),
        ("bad-default", Some(vec![0u8, 0, 0, 16, 1]), MeasurementScale::default(), false),
        ("bad-set", Some(vec![0u8, 0, 0, 16, 1]), scales[0].clone(), true),
        ("same", Some(an::write_scale_resource(&scales[0])), scales[0].clone(), false),
        ("changed", Some(an::write_scale_resource(&scales[0])), scales[2].clone(), true),
    ] {
        let mut d = ndoc();
        if let Some(r) = &raw {
            d.metadata.psd_resources.push((an::MEASUREMENT_SCALE, String::new(), std::sync::Arc::new(r.clone())));
        }
        d.measurement.scale = scale.clone();
        if counts {
            d.measurement.count_groups.push(photocraft_doc::CountGroup { points: vec![[1.0, 1.0]], ..Default::default() });
        }
        println!(
            "scx\t{name}\t{}\t{}\t{}\t{} {} {} {}",
            raw.as_ref().map_or("-".into(), |r| b64(r)),
            j!(scale),
            counts,
            an::raw_scale(&d).map_or("none".into(), |s| j!(s)),
            an::keep_resource(&d, an::MEASUREMENT_SCALE),
            an::keep_resource(&d, an::COUNT_INFO),
            opt_b64(an::fresh_scale_resource(&d))
        );
    }
}

// ---------------------------------------------------------------------------------------------
// slices_map
// ---------------------------------------------------------------------------------------------

/// A document with two layers whose PSD ids are 5 and 7 (and a third sharing id 5).
fn slice_doc() -> Document {
    let mut d = Document::new("site.psd", Size::new(120, 90), ColorMode::Rgb, SampleType::U8);
    for (n, id) in [("Logo", 5u32), ("Nav", 7), ("Dup", 5)] {
        let mut l = Layer::raster(n, d.pixel_format());
        l.psd_id = Some(id);
        d.layers.push(l);
    }
    d
}

fn slices_s(doc: &Document, s: &Slices) -> String {
    let ids: Vec<photocraft_doc::LayerId> = doc.walk().into_iter().map(|(_, _, l)| l.id).collect();
    let mut s = s.clone();
    for sl in &mut s.list {
        sl.layer = sl.layer.map(|l| photocraft_doc::LayerId(ids.iter().position(|i| *i == l).map_or(999, |i| i as u64)));
    }
    j!(s)
}

fn sample_slices(doc: &Document) -> Slices {
    let ids: Vec<photocraft_doc::LayerId> = doc.layers.iter().map(|l| l.id).collect();
    Slices {
        group_name: String::new(),
        list: vec![
            Slice { id: 1, name: "hero".into(), rect: Rect::new(10, 10, 60, 40), url: "https://example.org".into(), alt: "Hero \u{e9}".into(), ..Default::default() },
            Slice { id: 2, origin: SliceOrigin::Layer, layer: Some(ids[0]), rect: Rect::new(70, 50, 100, 80), kind: SliceKind::NoImage, outsets: [1, 1, 1, 1], cell_text: "<b>x</b>".into(), cell_text_is_html: true, horizontal_align: 2, vertical_align: 3, background: Some([255, 1, 2, 3]), ..Default::default() },
            Slice { id: 9, group_id: 4, origin: SliceOrigin::Layer, layer: Some(ids[1]), rect: Rect::new(-5, 0, 3, 200), target: "_blank".into(), message: "msg".into(), ..Default::default() },
        ],
    }
}

fn slices() {
    let d = slice_doc();
    let ids: std::collections::HashMap<photocraft_doc::LayerId, u32> = [(d.layers[0].id, 5u32)].into_iter().collect();
    let mut cases: Vec<(&str, Document)> = Vec::new();
    let mut a = d.clone();
    a.slices = sample_slices(&d);
    cases.push(("sample", a.clone()));
    let mut b = a.clone();
    b.slices.group_name = "Group \u{1f600}".into();
    cases.push(("named", b));
    let mut c = d.clone();
    c.slices.list.push(Slice { id: u32::MAX, rect: Rect::new(10, 10, 20, 20), ..Default::default() });
    cases.push(("max-id", c));
    let mut e = d.clone();
    e.slices.list.push(Slice { id: u32::MAX, rect: Rect::new(0, 0, 120, 90), ..Default::default() });
    e.slices.list.push(Slice { id: 1, rect: Rect::new(0, 0, 1, 1), ..Default::default() });
    cases.push(("full", e));
    cases.push(("empty", d.clone()));
    for (name, doc) in &cases {
        let (res, w) = sm::export_resource(doc, &ids);
        let act = match &res {
            None => "keep".to_string(),
            Some(None) => "remove".to_string(),
            Some(Some(b)) => format!("write {}", b64(b)),
        };
        println!("slx\t{name}\t{}\t{act} {}", slices_s(doc, &doc.slices), w.map_or("-".into(), |w| format!("{w:?}")));
        if let Some(Some(b)) = &res {
            println!("slc\t{name}\t{}\t{}", b64(b), slices_s(&d, &sm::slices_from_psd(Some(b), &d)));
            // Imported back: unchanged, export keeps it.
            let mut back = doc.clone();
            back.slices = Slices::default();
            back.metadata.psd_resources.push((sm::SLICES, String::new(), std::sync::Arc::new(b.clone())));
            sm::import(&mut back);
            let (r2, _) = sm::export_resource(&back, &ids);
            println!("slx\t{name}/back\t{}\t{} {}", slices_s(&back, &back.slices), sm::unchanged(&back), if r2.is_none() { "keep" } else { "rewrite" });
        }
    }
    let (res, _) = sm::export_resource(&cases[0].1, &ids);
    if let Some(Some(b)) = res {
        for (s, m) in sweeps(&b) {
            println!("slc\tsample:{s}\t{}\t{}", b64(&m), slices_s(&d, &sm::slices_from_psd(Some(&m), &d)));
        }
        let v7 = photocraft_psd::slices::SlicesResource::from_bytes(&b).unwrap().to_descriptor_bytes();
        println!("slc\tv7\t{}\t{}", b64(&v7), slices_s(&d, &sm::slices_from_psd(Some(&v7), &d)));
    }
}

// ---------------------------------------------------------------------------------------------
// pattern_map
// ---------------------------------------------------------------------------------------------

fn pattern(mode: ColorMode, s: SampleType, alpha: bool) -> Pattern {
    let fmt = PixelFormat::new(mode, s, alpha);
    let mut surf = Surface::new(fmt);
    let n = fmt.channels();
    for y in 0..3 {
        for x in 0..5 {
            let px: Vec<f32> = (0..n).map(|c| ((x * 3 + y * 7 + c as i32 * 5) % 11) as f32 / 10.0).collect();
            surf.write_pixel(x, y, &px);
        }
    }
    Pattern::new("P", surf, 5, 3)
}

fn surf_s(s: &Surface, r: Rect) -> String {
    let f = s.format();
    format!("{:?}/{:?}/{}:{}", f.mode, f.sample, u8::from(f.alpha), h16(&s.to_interleaved(r)))
}

fn pat_s(p: &Pattern) -> String {
    format!("{:?}|{:?}|{}x{}|{}", p.id, p.name, p.width, p.height, surf_s(&p.surface, p.rect()))
}

fn pblk_res(b: &[u8]) -> String {
    match photocraft_psd::patterns::parse_pattern_block(b) {
        Ok(ps) => format!("ok [{}]", ps.iter().map(|p| pm::from_psd(p).map_or("none".into(), |p| pat_s(&p))).collect::<Vec<_>>().join(";")),
        Err(_) => "err".into(),
    }
}

fn psd_pats() -> Vec<(&'static str, PsdPattern)> {
    let base = |mode: u32, depth: u16, channels: Vec<Vec<u8>>| PsdPattern { mode, width: 4, height: 2, name: "N".into(), id: "id-1".into(), palette: None, depth, channels, alpha: None };
    let mut v = vec![
        ("gray1", base(1, 1, vec![vec![0b1010_0000, 0b0101_0000]])),
        ("gray1-short", base(1, 1, vec![vec![0xF0]])),
        ("indexed", PsdPattern { palette: Some((0..768).map(|i| (i * 7 % 256) as u8).collect()), ..base(2, 8, vec![vec![0, 1, 2, 3, 255, 100, 7, 9]]) }),
        ("indexed-nopal", base(2, 8, vec![vec![0; 8]])),
        ("duotone", base(8, 8, vec![vec![9; 8]])),
        ("multi", base(7, 8, vec![vec![1; 8], vec![2; 8]])),
        ("rgb-short", base(3, 8, vec![vec![1; 8], vec![2; 8]])),
        ("rgb-alpha", PsdPattern { alpha: Some(vec![128; 8]), ..base(3, 8, vec![vec![1; 8], vec![2; 4], vec![3; 8]]) }),
        ("lab16", base(9, 16, vec![(0..16).collect(), (16..32).collect(), vec![0x80; 16]])),
        ("cmyk32", base(4, 32, vec![[0.5f32.to_be_bytes(); 8].concat(), vec![0; 32], [1.0f32.to_be_bytes(); 8].concat(), [0.25f32.to_be_bytes(); 8].concat()])),
        ("zero", PsdPattern { width: 0, ..base(1, 8, vec![vec![]]) }),
    ];
    v.retain(|_| true);
    v
}

fn patterns() {
    for (name, p) in psd_pats() {
        if let Ok(b) = photocraft_psd::patterns::write_pattern_block(std::slice::from_ref(&p)) {
            println!("pblk\t{name}\t{}\t{}", b64(&b), pblk_res(&b));
        }
    }
    let mut all = Vec::new();
    for mode in [ColorMode::Rgb, ColorMode::Grayscale, ColorMode::Cmyk, ColorMode::Lab] {
        for s in [SampleType::U8, SampleType::U16, SampleType::F32] {
            for alpha in [false, true] {
                let p = pattern(mode, s, alpha);
                let psd = pm::to_psd(&p);
                let b = photocraft_psd::patterns::write_pattern_block(std::slice::from_ref(&psd)).unwrap();
                let back = pm::from_psd(&psd).map_or("none".into(), |b| pat_s(&b));
                println!("ptx\tto/{mode:?}/{s:?}/{alpha}\t{} {} {}", pat_s(&p), b64(&b), back);
                all.push(p);
            }
        }
    }
    // Indexed and other models are written as RGB.
    let p = pattern(ColorMode::Rgb, SampleType::U8, true);
    let fmt = PixelFormat::new(ColorMode::Indexed, SampleType::U8, false);
    let mut odd = Pattern::new("Odd \u{e9}", Surface::new(fmt), 2, 2);
    odd.surface.write_pixel(1, 1, &[0.2, 0.4, 0.6]);
    let psd = pm::to_psd(&odd);
    println!("ptx\tto/indexed\t{} {}", pat_s(&odd), b64(&photocraft_psd::patterns::write_pattern_block(std::slice::from_ref(&psd)).unwrap()));
    let raw = photocraft_psd::patterns::write_pattern_block(&[pm::to_psd(&p)]).unwrap();
    for (s, m) in sweeps(&raw) {
        println!("pblk\trgb:{s}\t{}\t{}", b64(&m), pblk_res(&m));
    }
    // Export of global blocks.
    let mut d = Document::new("x", Size::new(4, 4), ColorMode::Rgb, SampleType::U8);
    let txt: photocraft_doc::PsdGlobalBlock = (*b"8BIM", *b"Txt2", std::sync::Arc::new(vec![1]));
    d.metadata.psd_global_blocks = vec![txt.clone(), (*b"8BIM", *b"Patt", std::sync::Arc::new(raw.clone())), (*b"8BIM", *b"Pat2", std::sync::Arc::new(vec![0, 0, 0, 0]))];
    d.patterns = pm::from_global_blocks(&d);
    let ptx = |name: &str, d: &Document| {
        let out = pm::export_global_blocks(d);
        println!("ptx\texp/{name}\t[{}] {}", pm::from_global_blocks(d).iter().map(pat_s).collect::<Vec<_>>().join(";"), blocks_s(&out, &d.metadata.psd_global_blocks));
    };
    ptx("same", &d);
    d.patterns.push(pattern(ColorMode::Grayscale, SampleType::U16, true));
    ptx("added", &d);
    let mut d16 = d.clone();
    d16.depth = SampleType::F32;
    ptx("added32", &d16);
    d.patterns.clear();
    ptx("cleared", &d);
    let mut fresh = Document::new("x", Size::new(4, 4), ColorMode::Rgb, SampleType::U16);
    fresh.metadata.psd_global_blocks = vec![txt];
    fresh.patterns = all[..3].to_vec();
    ptx("fresh", &fresh);
    // .pat files.
    let pat = pm::write_pat(&all).unwrap();
    println!("ptx\twrite-pat\t{}", h16(&pat));
    let small = pm::write_pat(&all[..2]).unwrap();
    println!("ptx\twrite-pat2\t{}", b64(&small));
    let pat_res = |b: &[u8]| match pm::read_pat(b) {
        Ok(v) => format!("ok [{}]", v.iter().map(pat_s).collect::<Vec<_>>().join(";")),
        Err(e) => format!("err {e:?}"),
    };
    println!("pat\tall\t{}\t{}", b64(&pat), pat_res(&pat));
    for (s, m) in sweeps(&small) {
        println!("pat\tsmall:{s}\t{}\t{}", b64(&m), pat_res(&m));
    }
}

// ---------------------------------------------------------------------------------------------
// gradient_bake
// ---------------------------------------------------------------------------------------------

fn color_s(c: &Color) -> String {
    format!("{}:{}:{}:{}:{}:{}", c.mode as u8, fb(c.c[0]), fb(c.c[1]), fb(c.c[2]), fb(c.c[3]), fb(c.alpha))
}

fn stops_s(s: &[(f32, Color)]) -> String {
    s.iter().map(|(t, c)| format!("{}@{}", fb(*t), color_s(c))).collect::<Vec<_>>().join(";")
}

fn gradients() {
    use gradient_bake::{Method, bake};
    let lab = |l: f32, a: f32, b: f32| Color { mode: ColorMode::Lab, c: [l / 100.0, (a + 128.0) / 255.0, (b + 128.0) / 255.0, 0.0], alpha: 1.0 };
    let sets: Vec<(&str, Vec<(f32, Color)>)> = vec![
        ("bw", vec![(0.0, Color::BLACK), (1.0, Color::WHITE)]),
        ("orange-blue", vec![(0.0, Color::rgb(0.882, 0.602, 0.201)), (1.0, Color::rgb(0.164, 0.568, 0.851))]),
        ("three", vec![(0.7, Color::rgba(0.0, 1.0, 0.0, 0.5)), (0.1, Color::rgb(1.0, 0.0, 0.0)), (0.7, Color::rgb(0.0, 0.0, 1.0)), (1.0, Color::rgba(1.0, 1.0, 1.0, 0.25))]),
        ("lab", vec![(0.0, lab(19.07, 52.29, -85.08)), (1.0, lab(54.29, 80.81, 69.91))]),
        ("cmyk", vec![(0.0, Color { mode: ColorMode::Cmyk, c: [0.1, 0.9, 0.3, 0.0], alpha: 1.0 }), (0.6, Color::gray(0.5)), (1.0, lab(50.0, 0.0, 0.0))]),
        ("one", vec![(0.5, Color::WHITE)]),
        ("odd", vec![(-0.5, Color::rgb(2.0, -1.0, 0.5)), (f32::NAN, Color::WHITE), (1.5, Color::rgb(0.3, 0.3, 0.3)), (0.0, Color::BLACK)]),
    ];
    for (name, stops) in sets {
        for code in ["Clsc", "Perc", "Lnr "] {
            for (smooth, mids) in [(0.0f32, vec![0.5f32]), (1.0, vec![0.5]), (0.6, vec![0.25, 0.9]), (2.0, vec![]), (-1.0, vec![0.0004 + 0.5])] {
                let input = format!("{}|{}|{}|{}", code, fb(smooth), mids.iter().map(|v| fb(*v)).collect::<Vec<_>>().join(","), stops_s(&stops));
                let out = bake(stops.clone(), &mids, smooth, Method::from_code(Some(code.as_bytes())));
                println!("gb\t{name}/{code}/{}/{}\t{input}\t{}", fb(smooth), mids.len(), stops_s(&out));
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// abr_map
// ---------------------------------------------------------------------------------------------

fn sample(id: &str, w: u32, h: u32, depth: u16) -> photocraft_psd::abr::AbrSample {
    let bpp = usize::from(depth / 8);
    let data = (0..w as usize * h as usize).flat_map(|i| std::iter::repeat_n(if i % 2 == 0 { 255u8 } else { 0 }, bpp)).collect();
    photocraft_psd::abr::AbrSample { id: id.into(), width: w, height: h, depth, data }
}
fn t(s: &str) -> Value {
    Value::Text(UnicodeString::new_nul(s))
}
fn u(unit: &[u8; 4], v: f64) -> Value {
    Value::UnitFloat { unit: *unit, value: v }
}
fn prc(v: f64) -> Value {
    u(b"#Prc", v)
}
fn en(ty: &str, v: &str) -> Value {
    Value::Enumerated { type_id: Id::new(ty), value: Id::new(v) }
}
fn var(control: i32, jitter: f64, minimum: f64) -> Value {
    Value::Descriptor(Descriptor::new("brVr").with("bVTy", Value::Integer(control)).with("fStp", Value::Integer(40)).with("jitter", prc(jitter)).with("Mnm ", prc(minimum)))
}
fn sampled_tip(id: &str) -> Descriptor {
    Descriptor::new("sampledBrush")
        .with("Dmtr", u(b"#Pxl", 64.0))
        .with("Angl", u(b"#Ang", 30.0))
        .with("Rndn", prc(80.0))
        .with("Spcn", prc(40.0))
        .with("Intr", Value::Boolean(true))
        .with("flipX", Value::Boolean(true))
        .with("flipY", Value::Boolean(false))
        .with("sampledData", t(id))
}

fn full_preset() -> Descriptor {
    Descriptor::new("brushPreset")
        .with("Nm  ", t("Kitchen Sink"))
        .with("Brsh", Value::Descriptor(sampled_tip("$tip-1")))
        .with("useTipDynamics", Value::Boolean(true))
        .with("flipX", Value::Boolean(true))
        .with("flipY", Value::Boolean(false))
        .with("minimumDiameter", prc(25.0))
        .with("minimumRoundness", prc(10.0))
        .with("tiltScale", prc(0.0))
        .with("szVr", var(2, 20.0, 0.0))
        .with("angleDynamics", var(6, 10.0, 0.0))
        .with("roundnessDynamics", var(3, 5.0, 0.0))
        .with("useScatter", Value::Boolean(true))
        .with("Cnt ", Value::Integer(3))
        .with("bothAxes", Value::Boolean(true))
        .with("countDynamics", var(0, 50.0, 0.0))
        .with("scatterDynamics", var(1, 250.0, 0.0))
        .with("useTexture", Value::Boolean(true))
        .with("TxtC", Value::Boolean(true))
        .with("InvT", Value::Boolean(true))
        .with("textureScale", prc(150.0))
        .with("textureBrightness", Value::Integer(-75))
        .with("textureContrast", Value::Integer(25))
        .with("textureBlendMode", en("BlnM", "CBrn"))
        .with("textureDepth", prc(60.0))
        .with("minimumDepth", prc(20.0))
        .with("textureDepthDynamics", var(2, 30.0, 0.0))
        .with("Txtr", Value::Descriptor(Descriptor::new("Ptrn").with("Nm  ", t("Grit")).with("Idnt", t("pat-uuid"))))
        .with(
            "dualBrush",
            Value::Descriptor(
                Descriptor::new("dualBrush")
                    .with("useDualBrush", Value::Boolean(true))
                    .with("Flip", Value::Boolean(true))
                    .with("Brsh", Value::Descriptor(Descriptor::new("computedBrush").with("Dmtr", u(b"#Pxl", 12.0)).with("Hrdn", prc(50.0))))
                    .with("BlnM", en("BlnM", "hardMix"))
                    .with("Spcn", prc(30.0))
                    .with("Cnt ", Value::Integer(2))
                    .with("bothAxes", Value::Boolean(true))
                    .with("scatterDynamics", var(0, 120.0, 0.0)),
            ),
        )
        .with("useColorDynamics", Value::Boolean(true))
        .with("clVr", var(2, 40.0, 0.0))
        .with("H   ", prc(10.0))
        .with("Strt", prc(20.0))
        .with("Brgh", prc(30.0))
        .with("purity", prc(-50.0))
        .with("colorDynamicsPerTip", Value::Boolean(false))
        .with("usePaintDynamics", Value::Boolean(true))
        .with("opVr", var(2, 0.0, 10.0))
        .with("prVr", var(1, 15.0, 5.0))
        .with("wtVr", var(0, 0.0, 0.0))
        .with("useBrushPose", Value::Boolean(true))
        .with("brushPoseTiltX", Value::Integer(50))
        .with("brushPoseAngle", Value::Integer(-90))
        .with("overridePoseAngle", Value::Boolean(true))
        .with("Wtdg", Value::Boolean(true))
        .with("Nose", Value::Boolean(true))
        .with("Rpt ", Value::Boolean(true))
        .with("someFutureKey", Value::Boolean(true))
        .with(
            "toolOptions",
            Value::Descriptor(
                Descriptor::new("currentToolOptions")
                    .with("Opct", Value::Integer(80))
                    .with("flow", Value::Integer(60))
                    .with("smoothing", Value::Boolean(true))
                    .with("smoothingValue", Value::Integer(40))
                    .with("usePressureOverridesSize", Value::Boolean(true))
                    .with("smoothingCatchup", Value::Boolean(false))
                    .with("Md  ", en("BlnM", "Mltp")),
            ),
        )
}

fn grit(mode: u32, depth: u16) -> PsdPattern {
    let ch = |v: Vec<u8>| -> Vec<u8> {
        match depth {
            16 => v.iter().flat_map(|b| [*b, *b]).collect(),
            32 => v.iter().flat_map(|b| (f32::from(*b) / 255.0).to_be_bytes()).collect(),
            _ => v,
        }
    };
    let base = vec![0u8, 64, 128, 255, 255, 128, 64, 0];
    let n = photocraft_psd::patterns::mode_channels(mode);
    PsdPattern {
        mode,
        width: 4,
        height: 2,
        name: "Grit".into(),
        id: "pat-uuid".into(),
        palette: (mode == 2).then(|| (0..768).map(|i| (i % 256) as u8).collect()),
        depth,
        channels: (0..n).map(|c| ch(base.iter().map(|v| v.wrapping_add(c as u8 * 40)).collect())).collect(),
        alpha: None,
    }
}

fn abr_res(b: &[u8]) -> String {
    match abr_map::read_abr(b, "G") {
        Ok(imp) => format!("ok v={} P={} {} W{:?}", imp.version, imp.presets.len(), h16(serde_json::to_string(&imp.presets).unwrap().as_bytes()), imp.warnings),
        Err(e) => format!("err {e:?}"),
    }
}

fn abr_inputs() -> Vec<(String, Vec<u8>)> {
    use photocraft_psd::abr::{LegacyBrush, LegacyTip, write_v6, write_v12};
    let mut v = Vec::new();
    let computed = Descriptor::new("brushPreset").with("Nm  ", t("  Round 19 ")).with("Brsh", Value::Descriptor(Descriptor::new("computedBrush").with("Dmtr", u(b"#Pxl", 19.0)).with("Hrdn", prc(0.0)).with("Spcn", prc(10.0))));
    for sub in [1u16, 2] {
        v.push((format!("v6-{sub}"), write_v6(sub, &[sample("$tip-1", 16, 8, 8)], &[grit(1, 8)], &[full_preset(), computed.clone()], true).unwrap()));
    }
    let orphan = Descriptor::new("brushPreset").with("Nm  ", t("Orphan")).with("Brsh", Value::Descriptor(sampled_tip("$nowhere")));
    v.push(("missing".into(), write_v6(2, &[sample("$tip-1", 4, 4, 16), sample("$tip-2", 4, 4, 8)], &[], &[full_preset(), orphan.clone()], false).unwrap()));
    v.push(("single-sample".into(), write_v6(2, &[sample("$only", 5, 3, 16)], &[], &[orphan], false).unwrap()));
    for (k, (mode, depth)) in [(3u32, 8u16), (2, 8), (4, 16), (9, 32), (1, 16)].iter().enumerate() {
        let mut p = grit(*mode, *depth);
        if k == 4 {
            p.id = "other".into();
        }
        v.push((format!("texture-{mode}-{depth}"), write_v6(2, &[sample("$tip-1", 4, 4, 8)], &[p], &[full_preset()], false).unwrap()));
    }
    let brushes = vec![
        LegacyBrush { name: String::new(), spacing: 25, anti_alias: true, tip: LegacyTip::Computed { diameter: 13, hardness: 50, angle: 20, roundness: 60 } },
        LegacyBrush { name: "Dune Grass".into(), spacing: 0, anti_alias: false, tip: LegacyTip::Sampled(sample("", 30, 10, 8)) },
        LegacyBrush { name: " \u{e9} ".into(), spacing: 999, anti_alias: true, tip: LegacyTip::Computed { diameter: 0, hardness: 200, angle: -400, roundness: 0 } },
    ];
    for version in [1u16, 2] {
        v.push((format!("v{version}"), write_v12(version, &brushes, true).unwrap()));
    }
    v.push(("tips-only".into(), write_v6(1, &[sample("$a", 8, 8, 8), sample("$b", 3, 9, 16)], &[], &[], true).unwrap()));
    let tip = Descriptor::new("computedBrush").with("Dmtr", u(b"#Pxl", 30.0)).with("Spcn", prc(35.0)).with("Intr", Value::Boolean(false));
    let wet = Descriptor::new("brushPreset")
        .with("Nm  ", t("Wet Pen"))
        .with("Brsh", Value::Descriptor(tip))
        .with("useTipDynamics", Value::Boolean(true))
        .with("szVr", var(3, 0.0, 0.0))
        .with("angleDynamics", var(7, 1.0, 0.0))
        .with("tiltScale", prc(150.0))
        .with("brushProjection", Value::Boolean(true))
        .with("usePaintDynamics", Value::Boolean(true))
        .with("wtVr", var(2, 30.0, 10.0))
        .with("mxVr", var(1, 45.0, 0.0))
        .with("useBrushPose", Value::Boolean(true))
        .with("brushPoseTiltY", Value::Double(-300.0))
        .with("overridePoseTiltY", Value::Boolean(true))
        .with("toolOptions", Value::Descriptor(Descriptor::new("currentToolOptions").with("Ld  ", prc(20.0)).with("mixRatio", prc(165.0)).with("flow", prc(70.0)).with("Mrgd", Value::Boolean(true)).with("smoothing", Value::Boolean(false)).with("smoothingValue", Value::Integer(40)).with("smoothingRadiusMode", Value::Boolean(true))));
    let plain = Descriptor::new("brushPreset").with("Brsh", Value::Descriptor(Descriptor::new("computedBrush").with("Dmtr", Value::Double(f64::INFINITY))));
    v.push(("mixer".into(), write_v6(2, &[], &[], &[wet, plain], true).unwrap()));
    v.push(("big-tip".into(), write_v6(2, &[photocraft_psd::abr::AbrSample { id: "$big".into(), width: 3000, height: 15, depth: 8, data: vec![200; 3000 * 15] }], &[], &[], true).unwrap()));
    v
}

fn abr() {
    for (name, b) in abr_inputs() {
        println!("abr\t{name}\t{}\t{}", b64(&b), abr_res(&b));
        if name == "v6-2" || name == "v2" {
            for (s, m) in sweeps(&b) {
                println!("abr\t{name}:{s}\t{}\t{}", b64(&m), abr_res(&m));
            }
        }
    }
    for (name, b) in [("empty", Vec::new()), ("8bps", b"8BPS not a brush".to_vec()), ("v6-short", vec![0, 6, 0, 2])] {
        println!("abr\t{name}\t{}\t{}", b64(&b), abr_res(&b));
    }
    // Large generated inputs (built the same way on the C side; the hash pins the input bytes).
    let s = photocraft_psd::abr::AbrSample { id: "$big".into(), width: 3000, height: 1500, depth: 8, data: vec![200; 3000 * 1500] };
    let b = photocraft_psd::abr::write_v6(2, &[s], &[], &[], true).unwrap();
    println!("abrg\tbig\t{} {}", h16(&b), abr_res(&b));
    let mut p = grit(3, 8);
    p.width = 1500;
    p.height = 20;
    p.channels = (0..3).map(|c| (0..1500 * 20).map(|i| ((i * 7 + c * 3) % 256) as u8).collect()).collect();
    let b = photocraft_psd::abr::write_v6(2, &[sample("$tip-1", 4, 4, 8)], &[p], &[full_preset()], true).unwrap();
    println!("abr\tbig-pattern\t{}\t{}", b64(&b), abr_res(&b));
}

// ---------------------------------------------------------------------------------------------
// text styles and text import
// ---------------------------------------------------------------------------------------------

fn styles() -> TextStyles {
    use photocraft_doc::text_styles::diff_attrs;
    let mut s = TextStyles::default();
    s.character.push(CharacterStyleDef { id: 3, name: "Loud".into(), attrs: diff_attrs(&CharStyle { size_pt: 30.0, underline: true, ..Default::default() }, &CharStyle::default()) });
    s.character.push(CharacterStyleDef { id: 4, name: "Quiet \u{e9}".into(), attrs: diff_attrs(&CharStyle { size_pt: 8.0, italic: true, ..Default::default() }, &CharStyle::default()) });
    s.paragraph.push(ParagraphStyleDef { id: 1, name: "Middle".into(), para_attrs: diff_attrs(&ParagraphStyle { align: TextAlign::Center, ..Default::default() }, &ParagraphStyle::default()), ..Default::default() });
    s
}

fn text_layer(styles: &TextStyles, link: bool) -> TextLayer {
    let base = CharStyle { font_family: "Inter".into(), ..Default::default() };
    let loud = if link { styles.resolve_char(None, Some(3), "Inter") } else { base.clone() };
    let quiet = if link { styles.resolve_char(Some(1), Some(4), "Inter") } else { base.clone() };
    let para = if link { styles.resolve_para(Some(1)) } else { ParagraphStyle::default() };
    let mut t = TextLayer {
        text: "calm LOUD\nquiet".into(),
        runs: vec![TextRun { len: 5, style: base }, TextRun { len: 5, style: loud }, TextRun { len: 5, style: quiet }],
        paragraphs: vec![ParagraphRun { len: 10, style: para }, ParagraphRun { len: 5, style: ParagraphStyle::default() }],
        transform: photocraft_geom::Affine::translate(4.0, 40.0),
        ..Default::default()
    };
    t.sync_summary();
    t
}

fn run_refs(t: &TextLayer) -> String {
    let c: Vec<String> = t.runs.iter().map(|r| r.style.style_sheet.map_or("-".into(), |v| v.to_string())).collect();
    let p: Vec<String> = t.paragraphs.iter().map(|r| r.style.style_sheet.map_or("-".into(), |v| v.to_string())).collect();
    format!("{}/{}", c.join(","), p.join(","))
}

fn text_styles() {
    for (name, st, link) in [("linked", styles(), true), ("plain", TextStyles::default(), false), ("unlinked", styles(), false)] {
        let t = text_layer(&st, link);
        let tysh = photocraft_text::psd::build_tysh(&t, 72.0, None);
        let out = tsm::export_tysh(&tysh, &t, &st, 72.0);
        println!("ts\texport/{name}\t{}", opt_b64(out.clone()));
        let data = out.unwrap_or(tysh);
        // Import into a document whose existing styles share one name.
        for pre in [false, true] {
            let mut d = Document::new("t", Size::new(200, 80), ColorMode::Rgb, SampleType::U8);
            let mut l = photocraft_text::psd::text_layer_from_tysh(&data, 72.0).unwrap_or_default();
            l.psd_raw = Some(std::sync::Arc::new(data.clone()));
            d.layers.push(Layer::new("t", LayerContent::Text(l.clone())));
            let mut g = Layer::group("g", vec![Layer::new("t2", LayerContent::Text(l))]);
            g.name = "g".into();
            d.layers.push(g);
            if pre {
                d.text_styles.character.push(CharacterStyleDef { id: 9, name: "Loud".into(), attrs: Default::default() });
            }
            let w = tsm::import(&mut d);
            let refs: Vec<String> = d.walk().into_iter().filter_map(|(_, _, l)| if let LayerContent::Text(t) = &l.content { Some(run_refs(t)) } else { None }).collect();
            println!("ts\timport/{name}/{pre}\t{} {} W{:?}", j!(d.text_styles), refs.join(" "), w);
        }
    }
    // A document with styles exhausting the id space.
    let mut st = styles();
    st.character.push(CharacterStyleDef { id: u32::MAX, name: "Max".into(), attrs: Default::default() });
    st.paragraph.push(ParagraphStyleDef { id: u32::MAX, name: "MaxP".into(), ..Default::default() });
    let t = text_layer(&styles(), true);
    let tysh = photocraft_text::psd::build_tysh(&t, 72.0, None);
    let data = tsm::export_tysh(&tysh, &t, &styles(), 72.0).unwrap();
    let mut d = Document::new("t", Size::new(200, 80), ColorMode::Rgb, SampleType::U8);
    let mut l = photocraft_text::psd::text_layer_from_tysh(&data, 72.0).unwrap_or_default();
    l.psd_raw = Some(std::sync::Arc::new(data));
    d.layers.push(Layer::new("t", LayerContent::Text(l)));
    d.text_styles = TextStyles { character: st.character[2..].to_vec(), paragraph: st.paragraph[1..].to_vec(), ..Default::default() };
    let w = tsm::import(&mut d);
    println!("ts\texhausted\t{} W{:?}", j!(d.text_styles), w);
}

fn text_import() {
    let mut d = Document::new("t", Size::new(120, 60), ColorMode::Rgb, SampleType::U8);
    let mk = |text: &str, family: &str, ps: Option<&str>, size: f32| {
        let mut t = TextLayer { text: text.into(), runs: vec![TextRun { len: text.len(), style: CharStyle { font_family: family.into(), postscript_name: ps.map(Into::into), size_pt: size, ..Default::default() } }], transform: photocraft_geom::Affine::translate(3.0, 30.0), ..Default::default() };
        t.sync_summary();
        t
    };
    d.layers.push(Layer::new("plain", LayerContent::Text(mk("Hello", "Inter", None, 24.0))));
    d.layers.push(Layer::new("ps", LayerContent::Text(mk("Semi", "Inter Semi Bold Guess", Some("Inter-SemiBold"), 18.0))));
    d.layers.push(Layer::new("mono", LayerContent::Text(mk("code()", "JetBrains Mono", Some("JetBrainsMono-Regular"), 12.0))));
    d.layers.push(Layer::new("unknown", LayerContent::Text(mk("Fall back", "No Such Family", Some("NoSuchFont-Bold"), 14.0))));
    d.layers.push(Layer::new("huge", LayerContent::Text(mk("W", "Inter", None, 30000.0))));
    let mut cached = mk("kept", "Inter", Some("Inter-Medium"), 10.0);
    cached.cache = Some(Surface::new(PixelFormat::RGBA8));
    d.layers.push(Layer::group("grp", vec![Layer::new("cached", LayerContent::Text(cached)), Layer::new("inner", LayerContent::Text(mk("inner", "Inter", None, 9.0)))]));
    let mut warp = mk("Arc", "Inter", None, 20.0);
    warp.warp = Some(photocraft_doc::text::TextWarp { style: "warpArc".into(), value: 50.0, horizontal_distortion: 0.0, vertical_distortion: 0.0, horizontal: true });
    d.layers.push(Layer::new("warp", LayerContent::Text(warp)));
    for (name, mode, depth) in [("rgb8", ColorMode::Rgb, SampleType::U8), ("gray16", ColorMode::Grayscale, SampleType::U16), ("cmyk8", ColorMode::Cmyk, SampleType::U8)] {
        let mut dd = d.clone();
        dd.mode = mode;
        dd.depth = depth;
        dd.resolution_dpi = if depth == SampleType::U16 { 144.0 } else { 72.0 };
        text_import::prepare(&mut dd);
        let out: Vec<String> = dd
            .walk()
            .into_iter()
            .filter_map(|(_, _, l)| {
                let LayerContent::Text(t) = &l.content else { return None };
                let fams: Vec<String> = t.runs.iter().map(|r| format!("{:?}/{}/{}", r.style.font_family, r.style.weight, r.style.italic)).collect();
                let cache = t.cache.as_ref().map_or("-".into(), |s| {
                    let b = s.content_bounds();
                    format!("{}:{},{},{},{}", surf_s(s, b), b.x0, b.y0, b.x1, b.y1)
                });
                Some(format!("{:?}|{:?}|{}|{}", l.name, t.font_family, fams.join(","), cache))
            })
            .collect();
        println!("ti\t{name}\t{}", out.join(" "));
    }
}

// ---------------------------------------------------------------------------------------------
// corpus
// ---------------------------------------------------------------------------------------------

fn files_in(root: &std::path::Path, exts: &[&str]) -> Vec<std::path::PathBuf> {
    let mut out = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(d) = stack.pop() {
        let Ok(rd) = std::fs::read_dir(&d) else { continue };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                stack.push(p);
            } else if p.extension().and_then(|e| e.to_str()).is_some_and(|e| exts.iter().any(|x| e.eq_ignore_ascii_case(x))) {
                out.push(p);
            }
        }
    }
    out.sort();
    out
}

/// The maps applied to every block and resource of a parsed file (no document import).
fn corpus_blocks(f: &photocraft_psd::PsdFile) -> String {
    let (w, h) = (f.header.width, f.header.height);
    let mut parts = Vec::new();
    for r in &f.resources {
        let d = &r.data;
        match r.id {
            1025 | 2000..=2997 => parts.push(format!("r{}={}", r.id, h16(path_res(vm::path_from_resource(d, w, h)).as_bytes()))),
            1050 => parts.push(format!("r1050={}", h16(slices_s(&Document::new("c", Size::new(w, h), ColorMode::Rgb, SampleType::U8), &sm::slices_from_psd(Some(d), &Document::new("c", Size::new(w, h), ColorMode::Rgb, SampleType::U8))).as_bytes()))),
            1074 => parts.push(format!("r1074={}", an::scale_from_resource(d).map_or("none".into(), |s| j!(s)))),
            _ => {}
        }
    }
    for b in &f.global_blocks {
        match &b.key {
            b"Anno" => parts.push(format!("Anno={}", h16(anno_res(&b.data).as_bytes()))),
            b"Patt" | b"Pat2" | b"Pat3" => parts.push(format!("{}={}", String::from_utf8_lossy(&b.key), h16(pblk_res(&b.data).as_bytes()))),
            _ => {}
        }
    }
    let dpi = 72.0;
    for (i, l) in f.layer_info.iter().flat_map(|li| li.layers.iter()).enumerate() {
        for b in &l.blocks {
            let r = match &b.key {
                b"vmsk" | b"vsms" => vmsk_res(&b.data, w, h),
                b"vstk" => vstk_res(&b.data, dpi),
                b"vscg" => vscg_res(&b.data),
                b"vogk" => vogk_res(&b.data),
                b"artb" | b"artd" | b"abdd" => art_res(&b.data),
                _ => continue,
            };
            parts.push(format!("{i}{}={}", String::from_utf8_lossy(&b.key), h16(r.as_bytes())));
        }
    }
    parts.join(" ")
}

/// What the maps put into an imported document.
fn corpus_doc(d: &Document) -> String {
    let mut parts = Vec::new();
    parts.push(format!("paths={}", h16(d.paths.iter().map(|p| format!("{:?}{}", p.name, j!(p.path))).collect::<String>().as_bytes())));
    parts.push(format!("work={}", d.work_path.as_ref().map_or("-".into(), |p| h16(j!(p).as_bytes()))));
    parts.push(format!("notes={}", h16(j!(d.notes).as_bytes())));
    parts.push(format!("slices={}", h16(slices_s(d, &d.slices).as_bytes())));
    parts.push(format!("comps={}", h16(comps_s(d, &d.layer_comps, d.last_applied_comp, d.last_document_state.as_ref()).as_bytes())));
    parts.push(format!("styles={}", h16(j!(d.text_styles).as_bytes())));
    parts.push(format!("patterns={}", h16(d.patterns.iter().map(pat_s).collect::<String>().as_bytes())));
    parts.push(format!("scale={}", j!(d.measurement.scale)));
    let mut layers = Vec::new();
    for (i, (_, _, l)) in d.walk().into_iter().enumerate() {
        let mut s = String::new();
        if let Some(m) = &l.vector_mask {
            s.push_str(&format!("vm={}", h16(j!(m).as_bytes())));
        }
        match &l.content {
            LayerContent::Shape(sh) => s.push_str(&format!("sh={}", h16(format!("{}{}{}{}", j!(sh.path), j!(sh.fill), j!(sh.stroke), j!(sh.live)).as_bytes()))),
            LayerContent::Group(g) => {
                if let Some(a) = &g.artboard {
                    s.push_str(&format!("ab={}", h16(j!(a).as_bytes())));
                }
            }
            LayerContent::Text(t) => s.push_str(&format!("tx={}", run_refs(t))),
            _ => {}
        }
        if !s.is_empty() {
            layers.push(format!("{i}:{s}"));
        }
    }
    parts.push(format!("L[{}]", layers.join(",")));
    parts.join(" ")
}

fn corpus() {
    let root = std::path::Path::new("corpus");
    for dir in ["psd", "psd-tools", "photoshop"] {
        for p in files_in(&root.join(dir), &["psd", "psb"]) {
            let Ok(bytes) = std::fs::read(&p) else { continue };
            let Ok(f) = photocraft_psd::PsdFile::from_bytes(&bytes) else { continue };
            let s = corpus_blocks(&f);
            if !s.is_empty() {
                println!("corp\t{}\t{s}", p.display());
            }
            let r = corpus_doc(&photocraft_io::psd_to_document(&f).0);
            println!("corpi\t{}\t{r}", p.display());
        }
    }
}

pub fn run() {
    // Bundled fonts only, so text rendering doesn't depend on what is installed.
    std::env::set_var("PHOTOCRAFT_NO_SYSTEM_FONTS", "1");
    vector();
    comps();
    annotations();
    slices();
    patterns();
    gradients();
    abr();
    text_styles();
    text_import();
    corpus();
}
