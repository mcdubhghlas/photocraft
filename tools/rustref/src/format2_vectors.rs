//! C5 format manifest/convert/autosave reference vectors (`rustref format2-vectors`), checked by
//! tests/format/test_parity2.c. Lines are tab-separated; strings are Rust `{:?}`.
//!
//!   man NAME LEN BLAKE3               manifest.json written by PcraftWriter::save_zip (ids renumbered)
//!   objs NAME N BLAKE3                the bundle's object names (sorted, comma-joined)
//!   stats NAME STATS                  that save's SaveStats
//!   rt NAME RESULT                    load_from_bytes(save) == doc: equal / DIFFERENT / E"error"
//!   rsave NAME BLAKE3                 manifest of save(load(save(doc))) (a canonical dump of the load)
//!   saveerr NAME ERROR                docs whose save is refused
//!   bundle NAME LEN BASE64            whole ZIP bundles for a few docs (C loads them: Rust -> C)
//!   xh CASE BASE64 RESULT             a hand-made manifest over the "small" doc's objects
//!   xm POINTER OP RESULT              one mutation of the "sweep" doc's manifest
//!   key INPUT FILE                    Autosaver::bundle_path file name for a key (sanitize)
//!   info BASE64                       RecoveryInfo sidecar JSON for fixed values
//!   list CASE RESULT                  list_recovery over a synthetic recovery directory
//!
//! RESULT for loads: `E"error"`, or `ok:` + the first 16 hex digits of the BLAKE3 of the
//! manifest the loaded document saves to (or `okE"error"` when that save is refused).
//!
//! Cross-direction: with PC_FORMAT2_XREAD=DIR, loads DIR/NAME.pcraft and DIR/NAME.dir (written
//! by the C writer, see test_parity2.c) instead and prints `xread NAME RESULT`: `equal` when the
//! document equals what the reference loads from its own bundle of the same document.
//! With PC_FORMAT2_PRINT=NAME, prints that document's manifest text instead (debugging).
use std::sync::Arc;

use base64::Engine;
use photocraft_color::{BlendMode, Color, ColorMode, PixelFormat, SampleType};
use photocraft_doc::text::{CharStyle, ParagraphRun, ParagraphStyle, TextRun, TextShape, TextWarp};
use photocraft_doc::*;
use photocraft_format::zip::{ZipReader, ZipWriter};
use photocraft_format::{self as pf, PcraftWriter, SaveOptions};
use serde_json::{json, Value};

// ---------------------------------------------------------------------------
// The integration tests' synthetic documents (crates/format/tests/common/mod.rs)
// ---------------------------------------------------------------------------

struct Rng(u64);
impl Rng {
    fn new(s: u64) -> Self {
        Rng(s)
    }
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
    fn f(&mut self) -> f32 {
        (self.next() >> 40) as f32 / (1u64 << 24) as f32
    }
}

fn scribble(s: &mut Surface, seed: u64, hdr: bool) {
    let mut r = Rng::new(seed);
    let ch = s.channels();
    for &(x0, y0) in &[(-12, -7), (0, 0), (250, 250), (600, 30)] {
        for dy in 0..9 {
            for dx in 0..11 {
                let px: Vec<f32> = (0..ch).map(|_| if hdr { r.f() * 4.0 - 1.0 } else { r.f() }).collect();
                s.write_pixel(x0 + dx, y0 + dy, &px);
            }
        }
    }
}

fn blob(seed: u8, n: usize) -> Arc<Vec<u8>> {
    Arc::new((0..n).map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed)).collect())
}

fn vector_path() -> Path {
    Path::new(vec![
        Subpath {
            closed: true,
            op: PathOp::Combine,
            knots: vec![
                Knot::corner(1.5, 2.25),
                Knot::smooth(photocraft_geom::Point::new(40.0, 5.0), photocraft_geom::Point::new(30.0, 0.0), photocraft_geom::Point::new(50.0, 10.0)),
                Knot::corner(10.0, 60.0),
            ],
        },
        Subpath::polygon(&[(5.0, 5.0), (9.0, 5.0), (9.0, 9.0)]).with_op(PathOp::Subtract),
        Subpath::polyline(&[(0.0, 0.0), (3.0, 4.0)]).with_op(PathOp::Exclude),
        Subpath::polygon(&[(0.0, 0.0), (3.0, 4.0), (3.0, 0.0)]).with_op(PathOp::Intersect),
    ])
}

fn rich_doc(mode: ColorMode, depth: SampleType) -> Document {
    let hdr = depth == SampleType::F32;
    let mut d = Document::with_background("Rich", Size::new(300, 280), mode, depth, Color::WHITE);
    d.resolution_dpi = 300.0;
    d.icc_profile = Some(blob(1, 500));
    let pf_ = d.pixel_format();

    let mut paint = Layer::raster("Paint", pf_);
    scribble(paint.surface_mut().unwrap(), 1, hdr);
    paint.blend = BlendMode::Multiply;
    paint.opacity = 0.75;
    paint.fill_opacity = 0.5;
    paint.clipped = true;
    paint.label = LabelColor::Violet;
    paint.locks.pixels = true;
    let mut mask = LayerMask::hide_all();
    scribble(&mut mask.surface, 2, false);
    mask.density = 0.8;
    mask.feather = 3.5;
    mask.linked = false;
    paint.mask = Some(mask);
    paint.effects.items.push(Effect::ColorOverlay { common: FxCommon::new(BlendMode::Normal, 0.8), color: Color::rgb(1.0, 0.2, 0.1) });
    paint.effects.items.push(Effect::GradientOverlay { common: FxCommon::new(BlendMode::Normal, 0.8), gradient: Gradient::default(), dither: true });
    paint.effects.psd_raw = Some(blob(3, 64));
    paint.psd_blocks.push((*b"vmsk", blob(4, 40)));
    paint.psd_blocks.push((*b"lnsr", blob(5, 4)));
    paint.psd_id = Some(42);

    let mut inner = Layer::raster("Inner", pf_);
    scribble(inner.surface_mut().unwrap(), 6, hdr);
    let nested = Layer::group("Nested", vec![inner]);
    let mut adj = Layer::new(
        "Hue/Sat",
        LayerContent::Adjustment(Adjustment::HueSaturation {
            hue: 30.0,
            saturation: -10.0,
            lightness: 5.0,
            colorize: false,
            ranges: photocraft_doc::adjust::HueRange::defaults(),
        }),
    );
    adj.visible = false;
    let mut group = Layer::group("Group", vec![nested, adj]);
    if let LayerContent::Group(g) = &mut group.content {
        g.expanded = false;
    }

    let mut fill = Layer::new("Fill", LayerContent::Fill(Fill::Solid(Color::rgba(0.1, 0.2, 0.3, 0.9))));
    let mut fc = Surface::new(pf_);
    scribble(&mut fc, 7, false);
    fill.fill_cache = Some(FillCache { fill: Fill::Solid(Color::rgba(0.1, 0.2, 0.3, 0.9)), surface: fc });
    let grad = Layer::new(
        "Gradient",
        LayerContent::Fill(Fill::Gradient {
            stops: vec![(0.0, Color::BLACK), (1.0, Color::WHITE)],
            angle: 45.0,
            scale: 1.5,
            style: GradientStyle::default(),
            reverse: true,
            opacity_stops: vec![(0.0, 1.0), (1.0, 0.25)],
            midpoints: vec![0.3],
            offset: (0.1, -0.2),
            dither: true,
            align: false,
        }),
    );

    let mut cache = Surface::new(pf_);
    scribble(&mut cache, 8, false);
    let text = Layer::new(
        "Type",
        LayerContent::Text(TextLayer {
            text: "Hello ✓ world".into(),
            font_family: "Inter".into(),
            size_pt: 24.0,
            color: Color::rgb(0.2, 0.4, 0.6),
            transform: Affine::translate(10.5, 20.25),
            cache: Some(cache.clone()),
            psd_raw: Some(blob(9, 100)),
            runs: vec![
                TextRun { len: 1, style: CharStyle { kerning: text::Kerning::Off, kern: 120.0, ..CharStyle::default() } },
                TextRun { len: 5, style: CharStyle::default() },
                TextRun { len: 9, style: CharStyle { kerning: text::Kerning::Optical, ..CharStyle::default() } },
            ],
            paragraphs: vec![ParagraphRun { len: 15, style: ParagraphStyle::default() }],
            shape: TextShape::Box { x: 1.0, y: 2.0, width: 100.0, height: 50.0 },
            orientation: text::Orientation::Vertical,
            antialias: text::AntiAlias::Crisp,
            warp: Some(TextWarp { style: "warpArc".into(), value: 25.0, ..Default::default() }),
        }),
    );
    let mut shape = Layer::new(
        "Shape",
        LayerContent::Shape(ShapeLayer {
            path: vector_path(),
            fill: Some(Fill::Solid(Color::BLACK)),
            stroke: Some(ShapeStroke {
                width: 2.5,
                paint: Fill::Solid(Color::rgb(0.0, 0.5, 1.0)),
                opacity: 0.75,
                align: StrokeAlign::Outside,
                cap: LineCap::Round,
                join: LineJoin::Bevel,
                miter_limit: 4.0,
                dashes: vec![2.0, 1.0],
                dash_offset: 0.5,
            }),
            live: Some(LiveShape::Rect { rect: [1.0, 2.0, 30.0, 40.0], radii: [4.0, 0.0, 4.0, 0.0] }),
            cache: Some(cache.clone()),
            psd_raw: Some(blob(10, 30)),
        }),
    );
    shape.label = LabelColor::Green;
    let mut vm = VectorMask::new(vector_path());
    vm.enabled = false;
    vm.density = 0.4;
    vm.feather = 3.0;
    vm.path.inverted = true;
    paint.vector_mask = Some(vm);
    let smart = Layer::new(
        "Smart",
        LayerContent::Smart(SmartObject {
            source: SmartSource::Embedded { file_name: "inner.png".into(), bytes: blob(11, 2000) },
            transform: Affine::scale(0.5),
            smart_filters: vec![SmartFilter {
                command: "filter.blur.gaussian".into(),
                params: json!({"radius": 2.5, "nested": {"a": [1, 2, 3]}}),
                blend: BlendMode::Screen,
                opacity: 0.6,
                visible: true,
            }],
            cache: Some(cache),
            psd_raw: None,
            filters_enabled: false,
            filter_mask: Some(LayerMask {
                surface: Surface::with_default(PixelFormat::GRAY8, &[1.0]),
                enabled: true,
                linked: false,
                density: 0.75,
                feather: 0.0,
            }),
            warp: Some(photocraft_geom::warp::Warp::custom(photocraft_geom::warp::BezierMesh::identity([0.0, 0.0, 48.0, 24.0], 2, 1), [0.0, 0.0, 48.0, 24.0])),
            stack_mode: Some(photocraft_doc::StackMode::Median),
            perspective: Some([1.0, 0.1, 2.0, 0.05, 1.0, 3.0, 0.001, 0.0005, 1.0]),
        }),
    );
    let linked = Layer::new(
        "Linked",
        LayerContent::Smart(SmartObject {
            source: SmartSource::Linked { path: "/tmp/linked.psd".into() },
            transform: Affine::IDENTITY,
            smart_filters: vec![],
            cache: None,
            psd_raw: Some(blob(12, 8)),
            filters_enabled: true,
            filter_mask: None,
            warp: None,
            stack_mode: None,
            perspective: None,
        }),
    );
    d.layers.extend([paint, group, fill, grad, text, shape, smart, linked]);

    let mut ch = Surface::new(PixelFormat::GRAY8.with_sample(depth));
    scribble(&mut ch, 13, false);
    let mut alpha = AlphaChannel::new("Alpha 1", ch.clone());
    alpha.color = Color::rgb(0.0, 0.5, 1.0);
    alpha.opacity = 0.35;
    alpha.indicates = photocraft_doc::ColorIndicates::SelectedAreas;
    d.channels.push(alpha);
    d.channels.push(AlphaChannel { spot: Some((Color::rgb(1.0, 0.0, 0.5), 0.7)), ..AlphaChannel::new("Spot", ch.clone()) });
    d.quick_mask = Some(AlphaChannel::new("Quick Mask", ch));
    let mut pat = Surface::new(PixelFormat::new(ColorMode::Rgb, SampleType::U16, true));
    scribble(&mut pat, 21, false);
    d.patterns.push(photocraft_doc::Pattern::new("$$$/Patterns/Test=Scribble", pat, 32, 24));
    d.color_table = Some(photocraft_doc::ColorTable { colors: vec![[0, 0, 0], [255, 128, 0]], transparent: Some(1) });
    d.duotone = Some(photocraft_doc::Duotone {
        inks: vec![photocraft_doc::DuotoneInk::new("Black", [0.0; 3]), photocraft_doc::DuotoneInk::new("PANTONE 151 C", [1.0, 0.5, 0.0])],
        psd_raw: Some(vec![1, 2, 3]),
    });
    d.guides = Guides { horizontal: vec![10.0, 20.5], vertical: vec![100.25] };
    let mut sel = Surface::new(PixelFormat::GRAY8);
    scribble(&mut sel, 14, false);
    d.selection = Some(sel);
    d.metadata.xmp = Some("<x:xmpmeta/>".into());
    d.metadata.exif = Some(blob(15, 64));
    d.metadata.psd_resources.push((1036, "thumb".into(), blob(16, 20)));
    d.metadata.psd_global_blocks.push((*b"8BIM", *b"Patt", blob(17, 50)));
    d.global_light = GlobalLight { angle: 120.0, altitude: 30.0 };
    d.paths.push(NamedPath { name: "Path 1".into(), path: vector_path(), psd_raw: Some(blob(18, 52)) });
    d.paths.push(NamedPath { name: "Empty".into(), path: Path::default(), psd_raw: None });
    let mut wp = vector_path();
    wp.fill_rule = FillRule::EvenOdd;
    d.work_path = Some(wp);
    d.clipping_path = Some(ClippingPath { name: "Path 1".into(), flatness: 2.0 });
    d
}

// ---------------------------------------------------------------------------
// More documents (shared JSON literals: test_parity2.c builds them from the same text)
// ---------------------------------------------------------------------------

const VARIABLES: &str = r#"{"defs":[{"name":"show","layer":101,"kind":{"type":"visibility"}},{"name":"title","layer":104,"kind":{"type":"textReplacement"}},{"name":"photo","layer":100,"kind":{"type":"pixelReplacement","method":"fit","align":"center","clip":false}}],"data_sets":[{"name":"Set 1","values":[{"variable":"show","kind":"visibility","value":true},{"variable":"title","kind":"text","value":"Hi"}]}],"active":0}"#;
const TIMELINE: &str = r#"{"fps":25.0,"duration":48,"current":3,"work_start":1,"work_end":40}"#;
const MEASUREMENT: &str = r#"{"scale":{"pixel_length":72.0,"logical_length":1.0,"units":"inches"},"count_groups":[{"name":"Cells","color":{"mode":"Rgb","c":[0.0,0.5,1.0,0.0],"alpha":1.0},"marker_size":3,"label_size":12,"visible":true,"points":[[1.0,2.0],[5.5,6.5]]}],"active_count_group":0,"ruler":{"start":[1.0,1.0],"end":[30.0,20.0],"protractor":[1.0,25.0]}}"#;
const NOTES: &str = r#"[{"author":"A","color":{"mode":"Rgb","c":[1.0,1.0,0.51,0.0],"alpha":1.0},"text":"Fix the sky\nplease","position":[10.0,12.0],"popup":[10.0,30.0,200.0,150.0],"open":true,"modified":""}]"#;
const TEXT_STYLES: &str = r#"{"character":[{"id":1,"name":"Big","attrs":{"size_pt":40.0,"underline":true}}],"paragraph":[{"id":7,"name":"Head","para_attrs":{"align":"Center"},"char_attrs":{"size_pt":20.0,"weight":700}}],"basic":{"id":0,"name":"Basic Paragraph","para_attrs":{},"char_attrs":{}},"psd_raw":{"k":[1,2]}}"#;
const SLICES: &str = r#"{"groupName":"Rich","list":[{"id":1,"groupId":0,"origin":"user","layer":null,"name":"Rich_01","rect":{"x0":0,"y0":0,"x1":10,"y1":10},"kind":"image","url":"","target":"","message":"","alt":"","cellTextIsHtml":false,"cellText":"","horizontalAlign":0,"verticalAlign":0,"background":null,"outsets":[0,0,0,0]},{"id":2,"groupId":0,"origin":"layer","layer":101,"name":"Rich_02","rect":{"x0":5,"y0":5,"x1":20,"y1":30},"kind":"image","url":"u","target":"","message":"","alt":"","cellTextIsHtml":false,"cellText":"","horizontalAlign":0,"verticalAlign":0,"background":[255,1,2,3],"outsets":[1,2,3,4]},{"id":3,"groupId":0,"origin":"layer","layer":999,"name":"Dangling","rect":{"x0":1,"y0":2,"x1":3,"y1":4},"kind":"noImage","url":"","target":"","message":"","alt":"","cellTextIsHtml":false,"cellText":"","horizontalAlign":0,"verticalAlign":0,"background":null,"outsets":[0,0,0,0]}]}"#;
const ARTBOARD: &str = r#"{"rect":{"x0":-1,"y0":-2,"x1":300,"y1":400},"background":{"custom":{"mode":"Rgb","c":[0.1,0.2,0.3,0.0],"alpha":0.5}},"preset":"iPhone 8/7/6"}"#;
const BEVEL: &str = r#"{"BevelEmboss":{"enabled":true,"style":"Emboss","technique":"ChiselHard","depth":1.2,"up":false,"size":9.0,"soften":1.0,"angle":30.0,"altitude":40.0,"use_global_light":false,"gloss_contour":"Linear","highlight":{"enabled":true,"blend":"Screen","opacity":0.7},"highlight_color":{"mode":"Rgb","c":[1.0,1.0,1.0,0.0],"alpha":1.0},"shadow":{"enabled":true,"blend":"Multiply","opacity":0.6},"shadow_color":{"mode":"Rgb","c":[0.0,0.0,0.0,0.0],"alpha":1.0},"contour":{"contour":"Linear","range":0.3,"anti_alias":true},"texture":{"name":"p","id":"i","scale":0.5,"depth":-1.5,"invert":true,"link":false,"phase":[1.0,2.0]}}}"#;
const BLEND_IF: &str = r#"{"ranges":[[{"black":[20,60],"white":[255,255]},{"black":[0,0],"white":[255,255]}],[{"black":[0,0],"white":[255,255]},{"black":[0,0],"white":[255,255]}],[{"black":[0,0],"white":[255,255]},{"black":[0,0],"white":[255,255]}],[{"black":[0,0],"white":[255,255]},{"black":[0,0],"white":[180,220]}]]}"#;
const ADVANCED: &str = r#"{"knockout":"deep","blendInterior":true,"blendClipped":false,"transparencyShapes":false,"layerMaskHidesEffects":true,"vectorMaskHidesEffects":true}"#;
const LIVE_SHAPES: &str = r#"[{"kind":"ellipse","rect":[10.0,10.0,20.0,30.0]},{"kind":"polygon","rect":[0.0,0.0,8.0,8.0],"sides":5,"star_ratio":0.5},{"kind":"line","from":[0.0,0.0],"to":[-1.0,1e-7],"weight":2.0}]"#;
const ADJUSTMENTS: &str = r#"[{"Exposure":{"exposure":0.5,"offset":0.01,"gamma":1.2}},{"Invert":null},{"Posterize":{"levels":4}},{"Threshold":{"level":128}}]"#;

macro_rules! from {
    ($s:expr) => {
        serde_json::from_str($s).unwrap()
    };
}

/// Renumber ids: the document 7, layers 100, 101, ... in pre-order (each layer before its
/// children), so manifests compare across processes.
fn renumber(d: &mut Document) {
    fn walk(layers: &mut [Layer], next: &mut u64) {
        for l in layers {
            l.id = LayerId(*next);
            *next += 1;
            if let LayerContent::Group(g) = &mut l.content {
                walk(&mut g.children, next);
            }
        }
    }
    d.id = DocId(7);
    let mut next = 100;
    walk(&mut d.layers, &mut next);
}

fn group_depth(layers: &[Layer]) -> usize {
    layers.iter().map(|l| if let LayerContent::Group(g) = &l.content { 1 + group_depth(&g.children) } else { 0 }).max().unwrap_or(0)
}

fn nested_doc(levels: usize, depth: SampleType) -> Document {
    let mut doc = rich_doc(ColorMode::Rgb, depth);
    let mut layers = std::mem::take(&mut doc.layers);
    for i in group_depth(&layers)..levels {
        layers = vec![Layer::group(format!("Level {i}"), layers)];
    }
    doc.layers = layers;
    doc
}

fn small_doc() -> Document {
    let mut d = Document::new("Small", Size::new(20, 10), ColorMode::Grayscale, SampleType::U8);
    let mut l = Layer::raster("A", d.pixel_format());
    l.surface_mut().unwrap().write_pixel(3, 4, &[0.5]);
    l.surface_mut().unwrap().write_pixel(300, 4, &[0.25]);
    l.psd_blocks.push((*b"lnsr", blob(5, 4)));
    d.layers.push(l);
    d.icc_profile = Some(blob(1, 10));
    d
}

/// Every document the vectors cover, ids renumbered.
fn docs() -> Vec<(String, Document)> {
    let mut out: Vec<(String, Document)> = Vec::new();
    for (mn, m) in [("rgb", ColorMode::Rgb), ("gray", ColorMode::Grayscale), ("cmyk", ColorMode::Cmyk), ("lab", ColorMode::Lab)] {
        for (dn, dp) in [("u8", SampleType::U8), ("u16", SampleType::U16), ("f32", SampleType::F32)] {
            out.push((format!("rich-{mn}-{dn}"), rich_doc(m, dp)));
        }
    }
    out.push(("empty".into(), Document::new("empty", Size::new(1, 1), ColorMode::Rgb, SampleType::U8)));
    out.push(("small".into(), small_doc()));

    // Comps, slices, variables, timeline, analysis, styles (ids below are renumbered values).
    let mut d = rich_doc(ColorMode::Rgb, SampleType::U8);
    renumber(&mut d);
    let fx = Effects {
        enabled: false,
        items: vec![Effect::ColorOverlay { common: FxCommon::new(BlendMode::Normal, 0.8), color: Color::rgb(1.0, 0.2, 0.1) }],
        psd_raw: Some(blob(30, 9)),
        reference: Some((1.5, 2.5)),
    };
    let comp_a = LayerComp {
        id: 1,
        name: "Comp A".into(),
        comment: "first".into(),
        apply_visibility: true,
        apply_position: true,
        apply_appearance: true,
        states: vec![
            CompLayerState {
                layer: LayerId(100),
                visible: Some(false),
                position: Some((3, -4)),
                appearance: Some(CompAppearance { blend: BlendMode::Screen, opacity: 0.5, fill_opacity: 0.25, effects: fx }),
            },
            CompLayerState { layer: LayerId(104), visible: Some(true), position: None, appearance: None },
            CompLayerState { layer: LayerId(5000), visible: None, position: Some((0, 0)), appearance: None },
        ],
    };
    let comp_b = LayerComp { id: 2, name: "B".into(), comment: String::new(), apply_visibility: false, apply_position: true, apply_appearance: false, states: vec![] };
    let last = LayerComp {
        id: 0,
        name: "Last Document State".into(),
        comment: String::new(),
        apply_visibility: true,
        apply_position: true,
        apply_appearance: true,
        states: vec![CompLayerState { layer: LayerId(102), visible: Some(true), position: Some((1, 2)), appearance: None }],
    };
    d.layer_comps = vec![comp_a, comp_b];
    d.last_applied_comp = Some(2);
    d.last_document_state = Some(last);
    d.variables = from!(VARIABLES);
    d.timeline = Some(from!(TIMELINE));
    d.measurement = from!(MEASUREMENT);
    d.notes = from!(NOTES);
    d.text_styles = from!(TEXT_STYLES);
    d.slices = from!(SLICES);
    if let LayerContent::Group(g) = &mut d.layers[2].content {
        g.artboard = Some(from!(ARTBOARD));
    }
    out.push(("extras".into(), d));

    // Advanced blending, Blend If, excluded channels, a bevel, link groups, effect reference.
    let mut d = rich_doc(ColorMode::Rgb, SampleType::U16);
    d.layers[0].excluded_channels = 0b101;
    d.layers[0].effects.items.push(from!(BEVEL));
    d.layers[0].effects.reference = Some((-3.25, 7.0));
    d.layers[0].blend_if = from!(BLEND_IF);
    d.layers[0].advanced = from!(ADVANCED);
    d.layers[1].advanced.knockout = Knockout::Shallow;
    d.layers[1].link_group = Some(3);
    d.layers[2].link_group = Some(3);
    out.push(("blending".into(), d));

    // Video layer and timeline.
    let mut d = Document::new("Vid", Size::new(8, 6), ColorMode::Rgb, SampleType::U8);
    d.timeline = Some(Timeline::new(3, 24.0));
    let fmt = d.pixel_format();
    let mut l = Layer::raster("Clip", fmt);
    let mut frames = Vec::new();
    for i in 0..3u8 {
        let mut s = Surface::new(fmt);
        let v = i as f32 / 2.0;
        s.fill_rect(photocraft_geom::Rect::new(0, 0, 8, 6), &photocraft_raster::from_rgba(&fmt, [v, 0.0, 1.0 - v, 1.0]));
        frames.push(s);
    }
    let mut vd = VideoData::new(frames, 24.0);
    vd.source = VideoSource::File { path: "/tmp/seq".into() };
    vd.show_altered = false;
    l.video = Some(vd);
    d.layers.push(l);
    out.push(("video".into(), d));

    // Every label colour on groups and children, at each depth.
    for (dn, depth) in [("u8", SampleType::U8), ("u16", SampleType::U16), ("f32", SampleType::F32)] {
        let mut d = Document::new("Labels", Size::new(2, 2), ColorMode::Rgb, depth);
        for color in LabelColor::ALL {
            let mut child = Layer::raster("Child", d.pixel_format());
            child.label = color;
            let mut group = Layer::group(color.label(), vec![child]);
            group.label = color;
            d.layers.push(group);
        }
        out.push((format!("labels-{dn}"), d));
    }

    // Shapes and adjustments of every flavour the shared literals give.
    let mut d = Document::new("Shapes", Size::new(64, 64), ColorMode::Rgb, SampleType::U8);
    let lives: Vec<LiveShape> = from!(LIVE_SHAPES);
    for (i, live) in lives.into_iter().enumerate() {
        d.layers.push(Layer::new(
            format!("S{i}"),
            LayerContent::Shape(ShapeLayer { path: vector_path(), fill: None, stroke: None, live: Some(live), cache: None, psd_raw: None }),
        ));
    }
    let adjs: Vec<Adjustment> = from!(ADJUSTMENTS);
    for (i, a) in adjs.into_iter().enumerate() {
        d.layers.push(Layer::new(format!("A{i}"), LayerContent::Adjustment(a)));
    }
    let mut smart = Layer::new(
        "Preset warp",
        LayerContent::Smart(SmartObject {
            source: SmartSource::Embedded { file_name: String::new(), bytes: Arc::new(Vec::new()) },
            transform: Affine::IDENTITY,
            smart_filters: vec![],
            cache: None,
            psd_raw: None,
            filters_enabled: true,
            filter_mask: None,
            warp: Some(photocraft_geom::warp::Warp::preset(photocraft_geom::warp::WarpStyle::ArcLower, 33.5, [0.0, 0.0, 10.0, 5.0])),
            stack_mode: None,
            perspective: None,
        }),
    );
    if let LayerContent::Smart(s) = &mut smart.content {
        if let Some(w) = &mut s.warp {
            w.h_distort = -12.0;
            w.v_distort = 4.0;
            w.vertical = true;
        }
    }
    d.layers.push(smart);
    d.channels.push(AlphaChannel::new("Empty", Surface::with_default(PixelFormat::GRAY8, &[0.5])));
    out.push(("shapes".into(), d));

    // Exhausted ids and maximal text runs (roundtrip.rs).
    let mut d = Document::new("exhausted", Size::new(2, 1), ColorMode::Rgb, SampleType::U8);
    d.slices.list.push(Slice { id: u32::MAX, ..Default::default() });
    d.text_styles.character.push(photocraft_doc::text_styles::CharacterStyleDef { id: u32::MAX, ..Default::default() });
    d.text_styles.paragraph.push(photocraft_doc::text_styles::ParagraphStyleDef { id: u32::MAX, ..Default::default() });
    d.layers.push(Layer::new(
        "Text",
        LayerContent::Text(TextLayer {
            text: "ab".into(),
            runs: vec![TextRun { len: 1, style: CharStyle::default() }, TextRun { len: usize::MAX, style: CharStyle { size_pt: 24.0, ..Default::default() } }],
            paragraphs: vec![ParagraphRun { len: 1, style: ParagraphStyle::default() }, ParagraphRun { len: usize::MAX, style: ParagraphStyle::default() }],
            ..Default::default()
        }),
    ));
    out.push(("exhausted".into(), d));

    out.push(("deep40".into(), nested_doc(40, SampleType::U8)));
    out.push(("deep100".into(), nested_doc(MAX_GROUP_DEPTH, SampleType::U16)));
    for (_, d) in &mut out {
        renumber(d);
    }
    out
}

/// Documents whose save is refused (non-finite values, too deep).
fn bad_docs() -> Vec<(String, Document)> {
    let mut out = Vec::new();
    let base = rich_doc(ColorMode::Rgb, SampleType::U8);
    let mut d = base.clone();
    d.insert_above(None, Layer::new("adj", LayerContent::Adjustment(Adjustment::Exposure { exposure: f32::NAN, offset: 0.0, gamma: 1.0 })));
    out.push(("nan-exposure".to_string(), d));
    let mut d = base.clone();
    d.insert_above(None, Layer::new("adj", LayerContent::Adjustment(Adjustment::Exposure { exposure: 0.0, offset: 0.0, gamma: f32::NEG_INFINITY })));
    out.push(("inf-gamma".to_string(), d));
    let mut d = base.clone();
    d.resolution_dpi = f32::INFINITY;
    out.push(("inf-dpi".to_string(), d));
    let mut d = base.clone();
    if let LayerContent::Smart(s) = &mut d.layers[7].content {
        s.perspective = Some([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, f64::NAN]);
    }
    out.push(("nan-perspective".to_string(), d));
    let mut d = base.clone();
    d.layers[0].effects.reference = Some((f64::INFINITY, 0.0));
    out.push(("inf-reference".to_string(), d));
    let mut d = base.clone();
    d.guides.vertical.push(f32::NAN);
    out.push(("nan-guide".to_string(), d));
    let mut d = base;
    d.channels[1].spot = Some((Color::rgb(1.0, 0.0, 0.5), f32::NAN));
    out.push(("nan-spot".to_string(), d));
    out.push(("deep101".to_string(), nested_doc(MAX_GROUP_DEPTH + 1, SampleType::U8)));
    for (_, d) in &mut out {
        renumber(d);
    }
    out
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn b3(b: &[u8]) -> String {
    blake3::hash(b).to_hex().to_string()
}

fn b3s(b: &[u8]) -> String {
    b3(b)[..16].to_string()
}

fn e(err: pf::FormatError) -> String {
    format!("E{:?}", err.to_string())
}

fn manifest_of(bundle: &[u8]) -> Vec<u8> {
    ZipReader::new(bundle).unwrap().read_by_name("manifest.json", 1 << 30).unwrap()
}

/// Rewrites a bundle with another manifest.
fn with_manifest(bundle: &[u8], manifest: &[u8]) -> Vec<u8> {
    let r = ZipReader::new(bundle).unwrap();
    let mut z = ZipWriter::new();
    for en in &r.entries {
        if en.name == "manifest.json" {
            z.add(&en.name, manifest).unwrap();
        } else {
            z.add(&en.name, &r.read(en, 1 << 30).unwrap()).unwrap();
        }
    }
    z.finish().unwrap()
}

/// `ok:` + the hash of what the loaded document saves to, or the error.
fn load_result(bundle: &[u8]) -> String {
    match pf::load_from_bytes(bundle) {
        Err(err) => e(err),
        Ok(doc) => match PcraftWriter::new().save_zip(&doc, &SaveOptions::default()) {
            Ok((b, _)) => format!("ok:{}", b3s(&manifest_of(&b))),
            Err(err) => format!("ok{}", e(err)),
        },
    }
}

fn stats(s: &pf::SaveStats) -> String {
    format!("{},{},{},{},{},{},{}", s.tiles_total, s.tiles_written, s.tiles_reused, s.blobs_total, s.blobs_written, s.objects_removed, s.manifest_bytes)
}

fn b64(b: &[u8]) -> String {
    base64::engine::general_purpose::STANDARD.encode(b)
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

fn doc_vectors() -> Vec<(String, Document, Vec<u8>)> {
    let mut saved = Vec::new();
    let print = std::env::var("PC_FORMAT2_PRINT").ok();
    for (name, doc) in docs() {
        let (bytes, st) = PcraftWriter::new().save_zip(&doc, &SaveOptions::default()).unwrap();
        let m = manifest_of(&bytes);
        if print.as_deref() == Some(name.as_str()) {
            println!("{}", String::from_utf8_lossy(&m));
        }
        let r = ZipReader::new(&bytes).unwrap();
        let names: Vec<&str> = r.entries.iter().map(|en| en.name.as_str()).filter(|n| *n != "manifest.json").collect();
        let joined = names.join(",");
        println!("man\t{name}\t{}\t{}", m.len(), b3(&m));
        println!("objs\t{name}\t{}\t{}", names.len(), b3(joined.as_bytes()));
        println!("stats\t{name}\t{}", stats(&st));
        let rt = match pf::load_from_bytes(&bytes) {
            Ok(back) if back == doc => "equal".to_string(),
            Ok(_) => "DIFFERENT".to_string(),
            Err(err) => e(err),
        };
        println!("rt\t{name}\t{rt}");
        println!("rsave\t{name}\t{}", load_result(&bytes));
        saved.push((name, doc, bytes));
    }
    for (name, doc) in bad_docs() {
        match PcraftWriter::new().save_zip(&doc, &SaveOptions::default()) {
            Ok(_) => println!("saveerr\t{name}\tok"),
            Err(err) => println!("saveerr\t{name}\t{}", e(err)),
        }
    }
    for (name, _, bytes) in &saved {
        if ["small", "rich-gray-u16", "video", "shapes"].contains(&name.as_str()) {
            println!("bundle\t{name}\t{}\t{}", bytes.len(), b64(bytes));
        }
    }
    saved
}

/// Hand-made manifests over the "small" doc's objects.
fn hostile(small: &[u8]) {
    let base: Value = serde_json::from_slice(&manifest_of(small)).unwrap();
    let mut cases: Vec<(String, Vec<u8>)> = Vec::new();
    let text = |v: &Value| serde_json::to_vec(v).unwrap();
    let edit = |f: &dyn Fn(&mut Value)| {
        let mut v = base.clone();
        f(&mut v);
        serde_json::to_vec(&v).unwrap()
    };
    for (n, ver) in [("v0", json!(0)), ("v1", json!(1)), ("v2", json!(2)), ("v999", json!(999)), ("vneg", json!(-1)), ("vfloat", json!(1.0)), ("vstr", json!("1")), ("vbig", json!(4294967296u64)), ("vmax", json!(4294967295u64)), ("vnull", Value::Null)] {
        cases.push((format!("version-{n}"), edit(&|v| v["format_version"] = ver.clone())));
    }
    cases.push(("version-missing".into(), edit(&|v| {
        v.as_object_mut().unwrap().remove("format_version");
    })));
    cases.push(("root-array".into(), b"[1,\"g\",{}]".to_vec()));
    cases.push(("root-string".into(), b"\"x\"".to_vec()));
    cases.push(("empty".into(), Vec::new()));
    cases.push(("trailing".into(), [text(&base), b" x".to_vec()].concat()));
    cases.push(("nan-literal".into(), String::from_utf8(text(&base)).unwrap().replacen("\"resolution_dpi\":72.0", "\"resolution_dpi\":NaN", 1).into_bytes()));
    cases.push(("huge-number".into(), String::from_utf8(text(&base)).unwrap().replacen("\"resolution_dpi\":72.0", "\"resolution_dpi\":1e999", 1).into_bytes()));
    cases.push(("f32-overflow".into(), edit(&|v| v["document"]["resolution_dpi"] = json!(1e300))));
    cases.push(("doc-array".into(), edit(&|v| v["document"] = json!([1, "x"]))));
    cases.push(("layers-object".into(), edit(&|v| v["document"]["layers"] = json!({}))));
    cases.push(("layers-missing".into(), edit(&|v| {
        v["document"].as_object_mut().unwrap().remove("layers");
    })));
    cases.push(("kind-unknown".into(), edit(&|v| v["document"]["layers"][0]["content"]["kind"] = json!("bogus"))));
    cases.push(("kind-missing".into(), edit(&|v| {
        v["document"]["layers"][0]["content"].as_object_mut().unwrap().remove("kind");
    })));
    cases.push(("kind-number".into(), edit(&|v| v["document"]["layers"][0]["content"]["kind"] = json!(3))));
    cases.push(("content-seq".into(), edit(&|v| {
        let s = v["document"]["layers"][0]["content"]["surface"].clone();
        v["document"]["layers"][0]["content"] = json!(["raster", s]);
    })));
    cases.push(("content-seq-long".into(), edit(&|v| {
        let s = v["document"]["layers"][0]["content"]["surface"].clone();
        v["document"]["layers"][0]["content"] = json!(["raster", s, 1]);
    })));
    cases.push(("layer-seq".into(), edit(&|v| {
        let l = v["document"]["layers"][0].clone();
        let o = l.as_object().unwrap();
        let order = ["id", "name", "visible", "locks", "blend", "opacity", "fill_opacity", "clipped", "mask", "vector_mask", "effects", "label", "content", "psd_blocks", "psd_id", "fill_cache"];
        v["document"]["layers"][0] = Value::Array(order.iter().map(|k| o[*k].clone()).collect());
    })));
    let group = |kids: Value| json!({"id": 50, "name": "G", "visible": true, "locks": {"transparency": false, "pixels": false, "position": false, "artboard": false, "all": false}, "blend": "PassThrough", "opacity": 1.0, "fill_opacity": 1.0, "clipped": false, "mask": null, "effects": {"enabled": true, "items": [], "psd_raw": null}, "label": "None", "content": {"kind": "group", "children": kids, "expanded": true}, "psd_blocks": [], "psd_id": null, "fill_cache": null});
    cases.push(("group-children-number".into(), edit(&|v| v["document"]["layers"] = json!([group(json!(5))]))));
    cases.push(("group-children-missing".into(), edit(&|v| {
        let mut g = group(json!([]));
        g["content"].as_object_mut().unwrap().remove("children");
        v["document"]["layers"] = json!([g]);
    })));
    cases.push(("group-bad-child-and-parent".into(), edit(&|v| {
        let mut g = group(json!([{"id": "x"}]));
        g["content"].as_object_mut().unwrap().remove("expanded");
        v["document"]["layers"] = json!([g]);
    })));
    cases.push(("raster-with-children".into(), edit(&|v| v["document"]["layers"][0]["content"]["children"] = json!([{"id": 1}]))));
    cases.push(("group-in-seq-content".into(), edit(&|v| {
        let l = v["document"]["layers"][0].clone();
        let mut g = group(json!([]));
        g["content"] = json!(["group", [l], false]);
        v["document"]["layers"] = json!([g]);
    })));
    let deep = |n: usize| {
        let mut g = base["document"]["layers"][0].clone();
        for _ in 0..n {
            g = group(json!([g]));
        }
        g
    };
    cases.push(("groups-100".into(), edit(&|v| v["document"]["layers"] = json!([deep(100)]))));
    cases.push(("groups-101".into(), edit(&|v| v["document"]["layers"] = json!([deep(101)]))));
    cases.push(("tile-dup".into(), edit(&|v| {
        let t = v["document"]["layers"][0]["content"]["surface"]["tiles"][0].clone();
        v["document"]["layers"][0]["content"]["surface"]["tiles"].as_array_mut().unwrap().push(t);
    })));
    for (n, tx) in [("tile-x-max", json!(8388606)), ("tile-x-over", json!(8388607)), ("tile-x-min", json!(-8388608)), ("tile-x-under", json!(-8388609)), ("tile-x-i32max", json!(2147483647)), ("tile-x-i64", json!(4294967296i64))] {
        cases.push((n.into(), edit(&|v| v["document"]["layers"][0]["content"]["surface"]["tiles"][0]["tx"] = tx.clone())));
    }
    for (n, h) in [("hash-short", json!("abc")), ("hash-upper", json!("A".repeat(64))), ("hash-slash", json!(format!("/{}", "0".repeat(63)))), ("hash-nul", json!(format!("{}\u{0}x", "0".repeat(64)))), ("hash-missing-object", json!("0".repeat(64)))] {
        cases.push((n.into(), edit(&|v| v["document"]["layers"][0]["content"]["surface"]["tiles"][0]["hash"] = h.clone())));
    }
    for (n, h) in [("default-odd", json!("0")), ("default-bad", json!("zz")), ("default-plus", json!("+f")), ("default-plusplus", json!("++")), ("default-long", json!("0000")), ("default-empty", json!("")), ("default-utf8", json!("é"))] {
        cases.push((n.into(), edit(&|v| v["document"]["layers"][0]["content"]["surface"]["default"] = h.clone())));
    }
    for (n, k) in [("key-short", json!("6c6e73")), ("key-bad", json!("6c6e737g")), ("key-long", json!("6c6e737201")), ("key-plus", json!("+c6e7372"))] {
        cases.push((n.into(), edit(&|v| v["document"]["layers"][0]["psd_blocks"][0][0] = k.clone())));
    }
    cases.push(("blob-bad".into(), edit(&|v| v["document"]["icc_profile"] = json!("../etc"))));
    cases.push(("blob-missing".into(), edit(&|v| v["document"]["icc_profile"] = json!("1".repeat(64)))));
    cases.push(("dup-keys".into(), String::from_utf8(text(&base)).unwrap().replacen("{\"composite\":null,", "{\"composite\":null,\"composite\":\"x\",\"thumbnail\":5,\"thumbnail\":null,", 1).into_bytes()));
    cases.push(("unknown-fields".into(), edit(&|v| {
        v["extra"] = json!({"deep": [1, 2]});
        v["document"]["layers"][0]["whatever"] = json!(null);
    })));
    cases.push(("label-unknown".into(), edit(&|v| v["document"]["layers"][0]["label"] = json!("Pink"))));
    cases.push(("blend-unknown".into(), edit(&|v| v["document"]["layers"][0]["blend"] = json!("Bogus"))));
    cases.push(("opacity-string".into(), edit(&|v| v["document"]["layers"][0]["opacity"] = json!("1"))));
    cases.push(("psd-id-big".into(), edit(&|v| v["document"]["layers"][0]["psd_id"] = json!(4294967296u64))));
    cases.push(("id-neg".into(), edit(&|v| v["document"]["layers"][0]["id"] = json!(-5))));
    cases.push(("id-float".into(), edit(&|v| v["document"]["id"] = json!(7.0))));
    cases.push(("comp-dangling".into(), edit(&|v| v["document"]["layer_comps"] = json!([{"id": 3, "name": "C", "apply_visibility": true, "apply_position": false, "apply_appearance": false, "states": [{"layer": 999, "visible": true}, {"layer": 100}]}]))));
    cases.push(("comp-huge-layer".into(), edit(&|v| v["document"]["layer_comps"] = json!([{"id": 3, "name": "C", "apply_visibility": true, "apply_position": false, "apply_appearance": false, "states": [{"layer": 18446744073709551615u64}]}]))));
    cases.push(("slice-dangling".into(), edit(&|v| v["document"]["slices"] = json!({"groupName": "g", "list": [{"id": 1, "origin": "layer", "layer": 12345}, {"id": 2, "origin": "auto", "layer": 777}, {"id": 3, "origin": "layer", "layer": 100}]}))));
    cases.push(("warp-bad-mesh".into(), edit(&|v| {
        v["document"]["layers"].as_array_mut().unwrap().push(json!({"id": 60, "name": "S", "visible": true, "locks": {"transparency": false, "pixels": false, "position": false, "artboard": false, "all": false}, "blend": "Normal", "opacity": 1.0, "fill_opacity": 1.0, "clipped": false, "mask": null, "effects": {"enabled": true, "items": [], "psd_raw": null}, "label": "None", "content": {"kind": "smart", "source": {"kind": "linked", "path": "/x"}, "transform": {"m": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0]}, "smart_filters": [], "cache": null, "psd_raw": null, "warp": {"style": "custom", "bounds": [0.0, 0.0, 1.0, 1.0], "mesh": {"us": [0.0, 1.0], "vs": [0.0, 1.0], "points": [[0.0, 0.0]]}}, "perspective": [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]}, "psd_blocks": [], "psd_id": null, "fill_cache": null}));
    })));
    cases.push(("ids-far-ahead".into(), edit(&|v| v["document"]["layers"][0]["id"] = json!(9000000000000u64))));
    cases.push(("source-kind-bad".into(), edit(&|v| {
        v["document"]["layers"].as_array_mut().unwrap().push(json!({"id": 61, "name": "S", "visible": true, "locks": {"transparency": false, "pixels": false, "position": false, "artboard": false, "all": false}, "blend": "Normal", "opacity": 1.0, "fill_opacity": 1.0, "clipped": false, "mask": null, "effects": {"enabled": true, "items": [], "psd_raw": null}, "label": "None", "content": {"kind": "smart", "source": {"kind": "folder"}, "transform": {"m": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0]}, "smart_filters": []}, "psd_blocks": [], "psd_id": null, "fill_cache": null}));
    })));
    // Tags of internally tagged enums nested in another (serde's Content buffer) also accept a
    // variant index; at the top level they don't.
    let layer_with = |id: u64, content: Value| json!({"id": id, "name": "T", "visible": true, "locks": {"transparency": false, "pixels": false, "position": false, "artboard": false, "all": false}, "blend": "Normal", "opacity": 1.0, "fill_opacity": 1.0, "clipped": false, "mask": null, "effects": {"enabled": true, "items": [], "psd_raw": null}, "label": "None", "content": content, "psd_blocks": [], "psd_id": null, "fill_cache": null});
    let blob_hash = base["document"]["layers"][0]["psd_blocks"][0][1].clone();
    for (n, tag) in [("0", json!(0)), ("1", json!(1)), ("2", json!(2)), ("neg", json!(-1)), ("float", json!(1.0)), ("bool", json!(true)), ("null", Value::Null)] {
        let src = json!({"kind": tag, "file_name": "f", "blob": blob_hash.clone(), "path": "/p"});
        let c = json!({"kind": "smart", "source": src, "transform": {"m": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0]}, "smart_filters": []});
        cases.push((format!("source-tag-{n}"), edit(&|v| v["document"]["layers"].as_array_mut().unwrap().push(layer_with(70, c.clone())))));
        let live = json!({"kind": tag, "rect": [0.0, 0.0, 1.0, 1.0], "radii": [0.0, 0.0, 0.0, 0.0], "sides": 3, "star_ratio": 0.5, "from": [0.0, 0.0], "to": [1.0, 1.0], "weight": 1.0});
        let c = json!({"kind": "shape", "fill": null, "cache": null, "psd_raw": null, "live": live});
        cases.push((format!("live-tag-{n}"), edit(&|v| v["document"]["layers"].as_array_mut().unwrap().push(layer_with(71, c.clone())))));
        let l = base["document"]["layers"][0].clone();
        let mut inner = l.clone();
        inner["content"]["kind"] = tag.clone();
        let c = json!(["group", [inner], false]);
        cases.push((format!("nested-content-tag-{n}"), edit(&|v| v["document"]["layers"] = json!([layer_with(72, c.clone())]))));
        let mut top = l.clone();
        top["content"]["kind"] = tag.clone();
        cases.push((format!("top-content-tag-{n}"), edit(&|v| v["document"]["layers"] = json!([top.clone()]))));
    }
    for (name, m) in cases {
        let b = with_manifest(small, &m);
        let r = if name == "ids-far-ahead" {
            // Fresh ids differ per process: only whether it loads, and the remapped layer id is new.
            match pf::load_from_bytes(&b) {
                Ok(d) => format!("ok:remapped={}", d.layers[0].id.0 != 9000000000000),
                Err(err) => e(err),
            }
        } else {
            load_result(&b)
        };
        println!("xh\t{name}\t{}\t{r}", b64(&m));
    }
}

/// Every node of the sweep manifest gets a few mutations (arrays: first and last element only).
fn sweep(bundle: &[u8]) {
    let base: Value = serde_json::from_slice(&manifest_of(bundle)).unwrap();
    let mut nodes: Vec<(String, Value)> = Vec::new();
    fn esc(k: &str) -> String {
        k.replace('~', "~0").replace('/', "~1")
    }
    fn walk(v: &Value, ptr: String, out: &mut Vec<(String, Value)>) {
        out.push((ptr.clone(), v.clone()));
        match v {
            Value::Object(o) => {
                for (k, c) in o {
                    walk(c, format!("{ptr}/{}", esc(k)), out);
                }
            }
            Value::Array(a) => {
                let n = a.len();
                for (i, c) in a.iter().enumerate() {
                    if i == 0 || i + 1 == n {
                        walk(c, format!("{ptr}/{i}"), out);
                    }
                }
            }
            _ => {}
        }
    }
    walk(&base, String::new(), &mut nodes);
    for (ptr, v) in &nodes {
        if ptr.is_empty() {
            continue;
        }
        let mut ops: Vec<(&str, Option<Value>)> = vec![("del", None), ("null", Some(Value::Null))];
        match v {
            Value::Number(_) => {
                ops.push(("str", Some(json!("x"))));
                ops.push(("neg", Some(json!(-1))));
                ops.push(("flt", Some(json!(1.5))));
                ops.push(("big", Some(json!(1e300))));
            }
            Value::String(_) => {
                ops.push(("int", Some(json!(7))));
                ops.push(("estr", Some(json!(""))));
            }
            Value::Bool(_) => ops.push(("int", Some(json!(1)))),
            Value::Array(a) => {
                ops.push(("obj", Some(json!({}))));
                ops.push(("earr", Some(json!([]))));
                if let Some(first) = a.first() {
                    let mut d = a.clone();
                    d.push(first.clone());
                    ops.push(("dup", Some(Value::Array(d))));
                }
            }
            Value::Object(_) => {
                ops.push(("arr", Some(json!([]))));
                ops.push(("eobj", Some(json!({}))));
            }
            Value::Null => {
                ops.push(("int", Some(json!(7))));
                ops.push(("str", Some(json!("x"))));
            }
        }
        for (op, val) in ops {
            let mut m = base.clone();
            let (parent, last) = ptr.rsplit_once('/').unwrap();
            let p = if parent.is_empty() { &mut m } else { m.pointer_mut(parent).unwrap() };
            let key = last.replace("~1", "/").replace("~0", "~");
            match (p, val) {
                (Value::Object(o), None) => {
                    o.remove(&key);
                }
                (Value::Array(a), None) => {
                    a.remove(key.parse::<usize>().unwrap());
                }
                (Value::Object(o), Some(nv)) => {
                    o.insert(key, nv);
                }
                (Value::Array(a), Some(nv)) => {
                    a[key.parse::<usize>().unwrap()] = nv;
                }
                _ => unreachable!(),
            }
            let b = with_manifest(bundle, &serde_json::to_vec(&m).unwrap());
            println!("xm\t{ptr}\t{op}\t{}", load_result(&b));
        }
    }
}

/// Autosave: key sanitising, the sidecar text, and list_recovery over synthetic directories.
fn autosave_vectors() {
    let root = std::env::temp_dir().join(format!("rustref-format2-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(&root).unwrap();
    for key in ["doc-1", "x y/z", "", "é", "a\u{1F600}b", "..", "-_", "日本", "A.b", "\u{0}", "tab\there", "untitled"] {
        let a = pf::Autosaver::new(&root, key);
        let p = a.bundle_path();
        println!("key\t{key:?}\t{:?}", p.file_name().unwrap().to_string_lossy());
        drop(a);
    }
    let info = pf::autosave::RecoveryInfo { key: "doc-x_1".into(), document_name: "Ünïcode \"name\"".into(), original_path: Some("/work/a.pcraft".into()), saved_at: 1_700_000_000, revision: 42 };
    println!("info\t{}", b64(&serde_json::to_vec_pretty(&info).unwrap()));
    let info = pf::autosave::RecoveryInfo { key: "k".into(), document_name: String::new(), original_path: None, saved_at: 0, revision: u64::MAX };
    println!("info\t{}", b64(&serde_json::to_vec_pretty(&info).unwrap()));

    // Each case: files to create (name, content); `B:<key>` makes <key>.pcraft/manifest.json.
    let side = |key: &str, at: u64| format!("{{\"key\":\"{key}\",\"document_name\":\"n\",\"original_path\":null,\"saved_at\":{at},\"revision\":1}}");
    let cases: Vec<(&str, Vec<(String, String)>)> = vec![
        ("order", vec![("a.json".into(), side("a", 5)), ("b.json".into(), side("b", 9)), ("c.json".into(), side("c", 5)), ("B:a".into(), String::new()), ("B:b".into(), String::new()), ("B:c".into(), String::new())]),
        ("name-mismatch", vec![("copy.json".into(), side("k", 1)), ("B:k".into(), String::new())]),
        ("escape", vec![("escape.json".into(), side("../k", 1)), ("B:k".into(), String::new())]),
        ("no-bundle", vec![("k.json".into(), side("k", 1))]),
        ("bundle-no-manifest", vec![("k.json".into(), side("k", 1)), ("D:k.pcraft".into(), String::new())]),
        ("manifest-dir", vec![("k.json".into(), side("k", 1)), ("D:k.pcraft/manifest.json".into(), String::new())]),
        ("junk", vec![("bogus.json".into(), "{}".into()), ("other.txt".into(), "x".into()), ("x.json".into(), "not json".into())]),
        ("dot-json", vec![(".json".into(), side("", 1)), ("B:".into(), String::new())]),
        ("upper-ext", vec![("k.JSON".into(), side("k", 1)), ("B:k".into(), String::new())]),
        ("extra-fields", vec![("k.json".into(), "{\"key\":\"k\",\"document_name\":\"n\",\"original_path\":\"/p\",\"saved_at\":3,\"revision\":2,\"more\":[1]}".into()), ("B:k".into(), String::new())]),
        ("missing-field", vec![("k.json".into(), "{\"key\":\"k\",\"document_name\":\"n\",\"saved_at\":3,\"revision\":2}".into()), ("B:k".into(), String::new())]),
        ("bad-types", vec![("k.json".into(), "{\"key\":\"k\",\"document_name\":\"n\",\"original_path\":null,\"saved_at\":-3,\"revision\":2}".into()), ("B:k".into(), String::new())]),
        ("trailing", vec![("k.json".into(), format!("{} x", side("k", 1))), ("B:k".into(), String::new())]),
        ("trailing-ws", vec![("k.json".into(), format!("{}\n\t ", side("k", 1))), ("B:k".into(), String::new())]),
        ("unsanitary-key", vec![("a b.json".into(), side("a b", 1)), ("B:a b".into(), String::new())]),
        ("ties", vec![("z.json".into(), side("z", 4)), ("y.json".into(), side("y", 4)), ("x.json".into(), side("x", 4)), ("B:x".into(), String::new()), ("B:y".into(), String::new()), ("B:z".into(), String::new())]),
        ("dir-named-json", vec![("D:k.json".into(), String::new()), ("B:k".into(), String::new())]),
        ("deep-json", vec![("k.json".into(), format!("{}{}", "[".repeat(200), "]".repeat(200))), ("B:k".into(), String::new())]),
    ];
    for (name, files) in cases {
        let dir = root.join(format!("list-{name}"));
        std::fs::create_dir_all(&dir).unwrap();
        for (f, content) in &files {
            if let Some(k) = f.strip_prefix("B:") {
                let b = dir.join(format!("{k}.pcraft"));
                std::fs::create_dir_all(&b).unwrap();
                std::fs::write(b.join("manifest.json"), b"{}").unwrap();
            } else if let Some(d) = f.strip_prefix("D:") {
                std::fs::create_dir_all(dir.join(d)).unwrap();
            } else {
                std::fs::write(dir.join(f), content).unwrap();
            }
        }
        let keys: Vec<String> = pf::list_recovery(&dir).iter().map(|en| format!("{:?}@{}", en.info.key, en.info.saved_at)).collect();
        println!("list\t{name}\t[{}]", keys.join(","));
    }
    println!("list\tmissing-dir\t[{}]", pf::list_recovery(&root.join("nope")).len());
    let _ = std::fs::remove_dir_all(&root);
}

fn xread(dir: &str) {
    for (name, doc) in docs() {
        // Compare with what the reference loads from its own bundle (a load remaps dangling
        // comp and slice layer ids, so "extras" doesn't round-trip to itself).
        let own = PcraftWriter::new().save_zip(&doc, &SaveOptions::default()).unwrap().0;
        let doc = pf::load_from_bytes(&own).unwrap();
        let p = std::path::Path::new(dir).join(format!("{name}.pcraft"));
        if !p.exists() {
            continue;
        }
        let r = match pf::load_path(&p) {
            Ok(back) if back == doc => "equal".to_string(),
            Ok(_) => "DIFFERENT".to_string(),
            Err(err) => e(err),
        };
        println!("xread\t{name}\t{r}");
        let pd = std::path::Path::new(dir).join(format!("{name}.dir"));
        if pd.exists() {
            let r = match pf::load_path(&pd) {
                Ok(back) if back == doc => "equal".to_string(),
                Ok(_) => "DIFFERENT".to_string(),
                Err(err) => e(err),
            };
            println!("xread\t{name}.dir\t{r}");
        }
    }
}

pub fn run() {
    if let Ok(dir) = std::env::var("PC_FORMAT2_XREAD") {
        xread(&dir);
        return;
    }
    let print = std::env::var("PC_FORMAT2_PRINT").is_ok();
    let saved = doc_vectors();
    if print {
        return;
    }
    let small = &saved.iter().find(|(n, _, _)| n == "small").unwrap().2;
    hostile(small);
    let sweep_bundle = &saved.iter().find(|(n, _, _)| n == "rich-gray-u8").unwrap().2;
    sweep(sweep_bundle);
    autosave_vectors();
}
