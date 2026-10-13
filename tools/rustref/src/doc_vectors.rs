//! `rustref doc-vectors`: document-model serde and logic vectors, checked line by line by
//! tests/doc/test_parity.c. One case per line, fields separated by tabs (no field contains a tab
//! or a newline):
//!
//!   ser  TYPE JSON          serde_json::to_string of a value; C: parse, from_json, to_json, write
//!   serp TYPE JSON PRETTY   to_string_pretty (newlines written as U+00B6), JSON is the compact form
//!   serv TYPE JSON VALUE    to_string(to_value(v)) (f32 widened, keys sorted)
//!   de   TYPE INPUT RESULT  from_value(from_str::<Value>(INPUT)): re-serialised, or "ERR <msg>"
//!                           (messages capped at 255 bytes like pc_error)
//!   fn   NAME ARGS... RESULT  logic vectors; floats are hex bits (f32 %08x, f64 %016x)
use std::sync::Arc;

use photocraft_color::{BlendMode, Color, ColorMode, PixelFormat, SampleType};
use photocraft_doc::adjust::{is_identity_curve, lut3d_len, CurvePoint, HueRange, LevelsChannel, ToneSpace};
use photocraft_doc::analysis::{CountGroup, Measurement, MeasurementScale, Note, Ruler};
use photocraft_doc::blend_if::{BlendIf, BlendRange};
use photocraft_doc::comps::{Artboard, ArtboardBackground, LayerComp};
use photocraft_doc::effects::*;
use photocraft_doc::mode::{ColorTable, Duotone, DuotoneInk, StackMode};
use photocraft_doc::pattern::{self, Pattern};
use photocraft_doc::slices::{self, Slice, SliceKind, SliceOrigin, Slices};
use photocraft_doc::text::*;
use photocraft_doc::text_styles::{self, CharacterStyleDef, ParagraphStyleDef, StyleAttrs, TextStyles};
use photocraft_doc::variables::{self, DataSet, DataValue, PixelAlign, PixelMethod, VarKind, VariableDef, Variables};
use photocraft_doc::vector::*;
use photocraft_doc::video::{Timeline, VideoSource};
use photocraft_doc::{
    Adjustment, AdvancedBlending, AlphaChannel, ColorIndicates, Document, Fill, Guides, Knockout, LabelColor, Layer, LayerContent,
    LayerId, LayerMask, Locks, SmartFilter, SmartObject, SmartSource, TextLayer,
};
use photocraft_geom::{Affine, Point, Rect, Size};
use photocraft_raster::Surface;
use serde_json::{json, Value as J};

const H0: u64 = 0xcbf29ce484222325;
fn fnv(h: &mut u64, b: &[u8]) {
    for &x in b {
        *h ^= x as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}
fn x32(v: f32) -> String { format!("{:08x}", v.to_bits()) }
fn x64(v: f64) -> String { format!("{:016x}", v.to_bits()) }
fn xs32(v: &[f32]) -> String { v.iter().map(|v| x32(*v)).collect::<Vec<_>>().join(",") }
fn trunc(mut m: String) -> String {
    while m.len() > 255 {
        m.pop();
    }
    m
}
fn line(parts: &[String]) {
    let mut s = String::from("fn");
    for p in parts {
        assert!(!p.contains('\t') && !p.contains('\n'), "{p:?}");
        s.push('\t');
        s.push_str(p);
    }
    println!("{s}");
}
macro_rules! l {
    ($($e:expr),+ $(,)?) => { line(&[$(($e).to_string()),+]) };
}
macro_rules! js {
    ($e:expr) => { serde_json::to_string(&$e).unwrap() };
}
fn opt<T: std::fmt::Display>(v: Option<T>) -> String { v.map_or("None".into(), |v| v.to_string()) }
fn optid(v: Option<u32>) -> String { v.map_or("-".into(), |v| v.to_string()) }

macro_rules! ser {
    ($t:ident: $($v:expr),+ $(,)?) => {{ $( {
        let v: $t = $v;
        let s = serde_json::to_string(&v).unwrap();
        assert!(!s.contains('\t'));
        println!("ser\t{}\t{}", stringify!($t), s);
    } )+ }};
}
macro_rules! serp {
    ($t:ident: $($v:expr),+ $(,)?) => {{ $( {
        let v: $t = $v;
        let p = serde_json::to_string_pretty(&v).unwrap();
        assert!(!p.contains('\u{b6}'));
        println!("serp\t{}\t{}\t{}", stringify!($t), serde_json::to_string(&v).unwrap(), p.replace('\n', "\u{b6}"));
    } )+ }};
}
macro_rules! serv {
    ($t:ident: $($v:expr),+ $(,)?) => {{ $( {
        let v: $t = $v;
        println!("serv\t{}\t{}\t{}", stringify!($t), serde_json::to_string(&v).unwrap(),
                 serde_json::to_string(&serde_json::to_value(&v).unwrap()).unwrap());
    } )+ }};
}
macro_rules! de {
    ($t:ident: $($inp:expr),+ $(,)?) => {{ $( {
        let inp: &str = $inp;
        let val: J = serde_json::from_str(inp).unwrap();
        let r = match serde_json::from_value::<$t>(val) {
            Ok(x) => serde_json::to_string(&x).unwrap(),
            Err(e) => format!("ERR {}", trunc(e.to_string())),
        };
        assert!(!r.contains('\t') && !r.contains('\n'));
        println!("de\t{}\t{}\t{}", stringify!($t), inp, r);
    } )+ }};
}

// ---------------------------------------------------------------------------------------------
// Values

const ESC: &str = "q\"b\\s/n\nr\rt\tc\u{1}\u{1f}d\u{7f} é 日本 😀 \u{2028}\u{0}";

fn sub1() -> f32 { f32::from_bits(1) }
fn subm() -> f32 { f32::from_bits(0x007f_ffff) }

fn colors() -> Vec<Color> {
    vec![
        Color::BLACK,
        Color::rgba(0.1, 1e-7, 1e20, 0.5),
        Color { mode: ColorMode::Cmyk, c: [-0.0, f32::MAX, sub1(), 0.3], alpha: 1.0 / 3.0 },
        Color { mode: ColorMode::Lab, c: [0.5, 0.25, subm(), 0.0], alpha: 0.0 },
        Color::gray(0.75),
        Color { mode: ColorMode::Duotone, c: [1e-45, 16777217.0, -1.5e-38, 123456789.0], alpha: 1e10 },
    ]
}

fn blend_modes() -> Vec<BlendMode> {
    let mut v = vec![BlendMode::PassThrough];
    v.extend(BlendMode::LAYER_MODES);
    v
}

fn curve(pts: &[(f32, f32)]) -> Vec<CurvePoint> { pts.iter().map(|&(i, o)| CurvePoint { input: i, output: o }).collect() }

fn custom_contour() -> Contour { Contour::Custom { name: "Cone – Inverted".into(), points: curve(&[(0.0, 1.0), (0.5, 0.1), (1.0, 0.0)]), corners: Vec::new() } }

fn gradient_full() -> Gradient {
    Gradient {
        stops: vec![(0.0, Color::rgb(1.0, 0.0, 0.0)), (0.1, colors()[2]), (1.0, Color::WHITE)],
        opacity_stops: vec![(0.0, 1.0), (0.3, 0.1), (1.0, -0.0)],
        style: GradientStyle::Diamond,
        angle: -45.5,
        scale: 1.5,
        reverse: true,
        align: false,
        offset: (0.1, -0.0),
    }
}

fn fxc(enabled: bool, blend: BlendMode, op: f32) -> FxCommon { FxCommon { enabled, blend, opacity: op } }

fn shadow2() -> Shadow {
    Shadow {
        common: fxc(false, BlendMode::Screen, 0.1),
        color: colors()[1],
        angle: -180.0,
        use_global_light: false,
        distance: 1e-7,
        spread: 0.3,
        size: 250.0,
        contour: custom_contour(),
        anti_alias: true,
        noise: 0.05,
        knocks_out: false,
    }
}

fn glow(paint: FxPaint) -> Glow {
    Glow {
        common: fxc(true, BlendMode::LinearDodge, 0.35),
        paint,
        technique: GlowTechnique::Precise,
        spread: 0.1,
        size: 18.0,
        contour: Contour::Linear,
        anti_alias: false,
        range: 0.5,
        jitter: 0.0,
        noise: 1e-7,
        source: GlowSource::Center,
    }
}

fn bevel(full: bool) -> Bevel {
    Bevel {
        enabled: !full,
        style: BevelStyle::PillowEmboss,
        technique: BevelTechnique::ChiselSoft,
        depth: 10.0,
        up: false,
        size: 5.0,
        soften: 0.1,
        angle: 120.0,
        altitude: 30.0,
        use_global_light: true,
        gloss_contour: if full { custom_contour() } else { Contour::Linear },
        highlight: fxc(true, BlendMode::Screen, 0.75),
        highlight_color: Color::WHITE,
        shadow: fxc(true, BlendMode::Multiply, 0.75),
        shadow_color: Color::BLACK,
        contour: if full { Some(BevelContour { contour: custom_contour(), range: 0.25, anti_alias: true }) } else { None },
        texture: if full { Some(bevel_texture()) } else { None },
    }
}

fn bevel_texture() -> BevelTexture {
    BevelTexture { name: "Bubbles".into(), id: "b7334da0-122f-11d4-8bb5-e27e45023b5f".into(), scale: 0.5, depth: -1.0, invert: true, link: false, phase: (3.5, -0.0) }
}

fn effects_all() -> Vec<Effect> {
    vec![
        Effect::default_drop_shadow(),
        Effect::InnerShadow(shadow2()),
        Effect::OuterGlow(glow(FxPaint::Color(Color::rgb(1.0, 1.0, 0.75)))),
        Effect::InnerGlow(glow(FxPaint::Gradient(Gradient::default()))),
        Effect::Stroke(StrokeFx { common: fxc(true, BlendMode::Normal, 1.0), size: 3.0, position: StrokePosition::Center, paint: FxPaint::Pattern { name: ESC.into(), id: "".into(), scale: 1.0 } }),
        Effect::ColorOverlay { common: fxc(false, BlendMode::Color, 0.5), color: colors()[3] },
        Effect::GradientOverlay { common: fxc(true, BlendMode::Overlay, 1.0), gradient: gradient_full(), dither: true },
        Effect::PatternOverlay { common: fxc(true, BlendMode::Normal, 1.0), name: "Water".into(), id: "id-1".into(), scale: 2.0, angle: 15.0, link: false, phase: (1.0, 2.0) },
        Effect::Satin(Satin { common: fxc(true, BlendMode::Multiply, 0.5), color: Color::BLACK, angle: 19.0, distance: 11.0, size: 14.0, contour: custom_contour(), anti_alias: true, invert: true }),
        Effect::BevelEmboss(bevel(true)),
        Effect::BevelEmboss(bevel(false)),
    ]
}

fn adjustments() -> Vec<Adjustment> {
    vec![
        Adjustment::BrightnessContrast { brightness: -150.0, contrast: 0.1, legacy: true },
        Adjustment::identity_levels(),
        Adjustment::Levels {
            master: LevelsChannel { in_black: 0.1, in_white: 0.9, gamma: 1e-7, out_black: -0.0, out_white: 1e20 },
            per_channel: [LevelsChannel::default(), LevelsChannel { gamma: 9.99, ..Default::default() }, LevelsChannel::default()],
            space: ToneSpace::Cmyk,
            black: LevelsChannel { out_white: 0.5, ..Default::default() },
        },
        Adjustment::identity_curves(),
        Adjustment::Curves { master: vec![], per_channel: [vec![], curve(&[(0.5, 0.5)]), vec![]], space: ToneSpace::Lab, black: curve(&[(0.0, 0.1), (1.0, 0.9)]), pencil: false },
        Adjustment::Exposure { exposure: 1.5, offset: -0.1, gamma: 1.0 },
        Adjustment::Vibrance { vibrance: 100.0, saturation: -100.0 },
        Adjustment::default_hue_saturation(),
        Adjustment::HueSaturation {
            hue: 180.0,
            saturation: -0.0,
            lightness: 0.1,
            colorize: true,
            ranges: [HueRange::neutral(3), HueRange { hue: 5.0, saturation: 1.0, lightness: 2.0, bounds: [315.0, 345.0, 15.0, 45.0] }, HueRange::neutral(0), HueRange::neutral(5), HueRange::neutral(1), HueRange::neutral(2)],
        },
        Adjustment::ColorBalance { shadows: [0.1, -0.2, 0.3], midtones: [0.0; 3], highlights: [1.0, 1e-45, -1.0], preserve_luminosity: true },
        Adjustment::BlackWhite { weights: [0.4, 0.6, 0.4, 0.6, 0.2, 0.8], tint: None },
        Adjustment::BlackWhite { weights: [0.0; 6], tint: Some([0.1, 0.2, 0.3]) },
        Adjustment::PhotoFilter { color: [0.925, 0.541, 0.0], density: 0.25, preserve_luminosity: false },
        Adjustment::ChannelMixer { matrix: [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.1], [0.3, 0.59, 0.11, -0.5]], monochrome: true },
        Adjustment::ColorLookup { name: "Fuji.cube".into(), lut: None, size: 0, tetrahedral: false, dither: false },
        Adjustment::ColorLookup { name: ESC.into(), lut: Some(Arc::new(vec![0.1, 1.0, -0.0, 1e20])), size: 33, tetrahedral: true, dither: true },
        Adjustment::ColorLookup { name: String::new(), lut: Some(Arc::new(vec![])), size: u32::MAX, tetrahedral: false, dither: true },
        Adjustment::Invert,
        Adjustment::Posterize { levels: 4 },
        Adjustment::Posterize { levels: u32::MAX },
        Adjustment::Threshold { level: 128.0 },
        Adjustment::GradientMap { stops: vec![], reverse: false, dither: false },
        Adjustment::GradientMap { stops: vec![(0.0, [0.0; 3]), (0.1, [1.0, 0.5, 0.25])], reverse: true, dither: true },
        Adjustment::SelectiveColor { relative: false, adjustments: [[0.1, 0.0, -0.1, 1.0]; 9] },
        Adjustment::Unsupported { psd_key: "blwh".into(), raw: vec![] },
        Adjustment::Unsupported { psd_key: "x\u{0}é".into(), raw: vec![0, 255, 7] },
    ]
}

fn fills() -> Vec<Fill> {
    vec![
        Fill::Solid(Color::rgb(0.2, 0.4, 0.6)),
        Fill::gradient(vec![(0.0, Color::BLACK), (1.0, Color::WHITE)], 90.0, 1.0, GradientStyle::Linear, false),
        Fill::Gradient {
            stops: vec![(0.25, colors()[2])],
            angle: -0.0,
            scale: 1e-7,
            style: GradientStyle::Reflected,
            reverse: true,
            opacity_stops: vec![(0.0, 0.5), (1.0, 1.0)],
            midpoints: vec![0.1, 0.9],
            offset: (0.1, -0.25),
            dither: true,
            align: false,
        },
        Fill::Pattern { name: ESC.into(), scale: 1.0, id: String::new(), angle: 0.0, link: true, phase: (0.0, 0.0) },
        Fill::Pattern { name: "Water".into(), scale: 0.5, id: "abc".into(), angle: 33.0, link: false, phase: (-1.5, 2.0) },
    ]
}

fn knots() -> Vec<Knot> {
    vec![
        Knot::corner(0.0, 0.0),
        Knot::smooth(Point::new(10.5, -3.25), Point::new(0.1, 1e-7), Point::new(1e20, -0.0)),
        Knot { anchor: Point::new(f64::MAX, 5e-324), in_ctrl: Point::new(1.0, 2.0), out_ctrl: Point::new(3.0, 4.0), smooth: false },
    ]
}

fn path_full() -> Path {
    Path {
        subpaths: vec![
            Subpath::polygon(&[(0.0, 0.0), (10.0, 0.0), (10.0, 10.0)]).with_op(PathOp::Subtract),
            Subpath { closed: false, knots: knots(), op: PathOp::Join },
            Subpath::polyline(&[(5.0, 5.0)]).with_op(PathOp::Intersect),
            Subpath { closed: true, knots: vec![], op: PathOp::Exclude },
            Subpath::polygon(&[(1.0, 1.0), (2.0, 2.0)]).with_op(PathOp::Join),
            Subpath::polygon(&[(-1.0, -2.0), (3.0, 4.0)]).with_op(PathOp::Combine),
        ],
        fill_rule: FillRule::EvenOdd,
        inverted: true,
    }
}

fn char_full() -> CharStyle {
    CharStyle {
        font_family: "Minion Pro".into(),
        font_style: "Bold Italic".into(),
        postscript_name: Some("MinionPro-BoldIt".into()),
        weight: 700,
        italic: true,
        size_pt: 0.1,
        color: colors()[2],
        tracking: -25.0,
        leading_pt: Some(14.4),
        baseline_shift_pt: -0.0,
        horizontal_scale: 1e-7,
        vertical_scale: 1e20,
        underline: true,
        strikethrough: true,
        faux_bold: true,
        faux_italic: true,
        kerning: Kerning::Optical,
        kern: 1.0 / 3.0,
        caps: Caps::SmallCaps,
        ligatures: false,
        discretionary_ligatures: true,
        features: vec![FontFeature { tag: "ss01".into(), value: 1 }, FontFeature { tag: "liga".into(), value: u16::MAX }],
        variations: vec![FontVariation { axis: "wght".into(), value: 0.1 }],
        language: Some("de-DE".into()),
        style_sheet: Some(u32::MAX),
    }
}

fn para_full() -> ParagraphStyle {
    ParagraphStyle {
        align: TextAlign::JustifyAll,
        first_line_indent_pt: 0.1,
        start_indent_pt: -0.0,
        end_indent_pt: 1e-7,
        space_before_pt: 1e20,
        space_after_pt: 3.4028235e38,
        auto_leading: 1.2,
        direction: TextDirection::Rtl,
        hyphenate: true,
        style_sheet: Some(0),
    }
}

fn attrs(v: J) -> StyleAttrs { v.as_object().unwrap().clone() }

fn styles_full() -> TextStyles {
    TextStyles {
        character: vec![
            CharacterStyleDef { id: 1, name: "Big".into(), attrs: attrs(json!({"size_pt": 40.0, "underline": true})) },
            CharacterStyleDef { id: u32::MAX, name: ESC.into(), attrs: StyleAttrs::new() },
        ],
        paragraph: vec![ParagraphStyleDef {
            id: 7,
            name: "Head".into(),
            para_attrs: attrs(json!({"align": "Center"})),
            char_attrs: attrs(json!({"size_pt": 20.0, "weight": 700, "color": {"mode": "Rgb", "c": [1.0, 0.0, 0.0, 0.0], "alpha": 1.0}})),
        }],
        basic: ParagraphStyleDef { id: 0, name: "Basic Paragraph".into(), para_attrs: attrs(json!({"auto_leading": 1.5})), char_attrs: attrs(json!({"font_family": "Inter"})) },
        psd_raw: Some(json!({"z": [1, 2.5, null, "x"], "a": {"k": true, "b": -3}, "big": 18446744073709551615u64})),
    }
}

fn slice_full() -> Slice {
    Slice {
        id: u32::MAX,
        group_id: 3,
        origin: SliceOrigin::Layer,
        layer: Some(LayerId(u64::MAX)),
        name: ESC.into(),
        rect: Rect::new(-5, 0, 100, i32::MAX),
        kind: SliceKind::Table,
        url: "https://example.com/?a=1&b=2".into(),
        target: "_blank".into(),
        message: "msg".into(),
        alt: "alt é".into(),
        cell_text_is_html: true,
        cell_text: "<b>x</b>".into(),
        horizontal_align: 2,
        vertical_align: u32::MAX,
        background: Some([255, 0, 128, 1]),
        outsets: [-1, 2, i32::MIN, i32::MAX],
    }
}

fn variables_full() -> Variables {
    Variables {
        defs: vec![
            VariableDef { name: "show".into(), layer: LayerId(1), kind: VarKind::Visibility },
            VariableDef { name: "title".into(), layer: LayerId(9007199254740993), kind: VarKind::TextReplacement },
            VariableDef { name: "photo".into(), layer: LayerId(u64::MAX), kind: VarKind::PixelReplacement { method: PixelMethod::AsIs, align: PixelAlign::BottomRight, clip: true } },
            VariableDef { name: "p2".into(), layer: LayerId(0), kind: VarKind::PixelReplacement { method: PixelMethod::Fit, align: PixelAlign::Center, clip: false } },
        ],
        data_sets: vec![
            DataSet {
                name: "row1".into(),
                values: vec![
                    DataValue { variable: "show".into(), value: variables::Value::Visibility(false) },
                    DataValue { variable: "title".into(), value: variables::Value::Text(ESC.into()) },
                    DataValue { variable: "photo".into(), value: variables::Value::Pixels("/tmp/a.png".into()) },
                ],
            },
            DataSet { name: "empty".into(), values: vec![] },
        ],
        active: Some(usize::MAX),
    }
}

fn measurement_full() -> Measurement {
    Measurement {
        scale: MeasurementScale { pixel_length: 0.1, logical_length: 1e20, units: "µm".into() },
        count_groups: vec![CountGroup::default(), CountGroup { name: "B".into(), color: colors()[3], marker_size: 0, label_size: u32::MAX, visible: false, points: vec![[1.0, 2.0], [-0.0, 5e-324]] }],
        active_count_group: 1,
        ruler: Some(Ruler { start: [0.0, 0.0], end: [3.0, 4.0], protractor: Some([-1.0, 0.1]) }),
    }
}

fn note_full() -> Note {
    Note { author: "A. N. Other".into(), color: Color::rgb(1.0, 1.0, 0.51), text: ESC.into(), position: [10.5, -2.0], popup: [0.0, 1.0, 200.0, 1e300], open: true, modified: "D:20240102030405Z".into() }
}

// ---------------------------------------------------------------------------------------------

fn ser_lines() {
    ser!(Size: Size::new(0, 0), Size::new(u32::MAX, 1));
    ser!(Rect: Rect::new(0, 0, 0, 0), Rect::new(i32::MIN, -5, i32::MAX, 7));
    ser!(Point: Point::new(0.1, -0.0), Point::new(1e20, 5e-324), Point::new(f64::MAX, -1e-7), Point::new(1.0, 2.5), Point::new(9007199254740993.0, 1e16));
    ser!(Affine: Affine::IDENTITY, Affine { m: [0.1, 1e-7, -0.0, 1e300, f64::MIN_POSITIVE, 123456.789] });
    for s in SampleType::ALL {
        ser!(SampleType: s);
    }
    for m in [ColorMode::Bitmap, ColorMode::Grayscale, ColorMode::Indexed, ColorMode::Rgb, ColorMode::Cmyk, ColorMode::Lab, ColorMode::Multichannel, ColorMode::Duotone] {
        ser!(ColorMode: m);
    }
    ser!(PixelFormat: PixelFormat::RGBA8, PixelFormat::new(ColorMode::Lab, SampleType::F32, false));
    for c in colors() {
        ser!(Color: c);
    }
    for b in blend_modes() {
        ser!(BlendMode: b);
    }
    ser!(Locks: Locks::default(), Locks { transparency: true, pixels: true, position: true, artboard: true, all: true }, Locks { pixels: true, artboard: true, ..Default::default() });
    for c in LabelColor::ALL {
        ser!(LabelColor: c);
    }
    for f in fills() {
        ser!(Fill: f);
    }
    ser!(SmartFilter:
        SmartFilter { command: "filter.gaussianBlur".into(), params: json!({"radius": 2.5, "b": [1, null, true], "a": {"z": "é", "y": -3}}), blend: BlendMode::Screen, opacity: 0.75, visible: false },
        SmartFilter { command: ESC.into(), params: J::Null, blend: BlendMode::Normal, opacity: 1.0, visible: true },
        SmartFilter { command: "x".into(), params: json!([18446744073709551615u64, -9223372036854775808i64, 1e-7, 0.1, -0.0, 1e300]), blend: BlendMode::PassThrough, opacity: 0.0, visible: true },
        SmartFilter { command: "y".into(), params: json!("str"), blend: BlendMode::Divide, opacity: sub1(), visible: true },
        SmartFilter { command: "z".into(), params: json!({}), blend: BlendMode::Luminosity, opacity: 0.5, visible: true },
    );
    ser!(Guides: Guides::default(), Guides { horizontal: vec![0.1, -0.0, 1e20], vertical: vec![f64::from(f32::MAX)], colors: Vec::new() });
    ser!(ColorIndicates: ColorIndicates::MaskedAreas, ColorIndicates::SelectedAreas);
    ser!(CurvePoint: CurvePoint { input: 0.1, output: 0.2 }, CurvePoint { input: -0.0, output: 1e-45 });
    ser!(LevelsChannel: LevelsChannel::default(), LevelsChannel { in_black: 0.1, in_white: 0.9, gamma: 1e-7, out_black: -0.0, out_white: 1e20 });
    ser!(ToneSpace: ToneSpace::Rgb, ToneSpace::Cmyk, ToneSpace::Lab);
    ser!(HueRange: HueRange::neutral(0), HueRange::neutral(5), HueRange { hue: -180.0, saturation: 100.0, lightness: -0.1, bounds: [315.0, 345.0, 15.0, 1e-7] });
    for a in adjustments() {
        ser!(Adjustment: a);
    }
    ser!(Knockout: Knockout::None, Knockout::Shallow, Knockout::Deep);
    ser!(AdvancedBlending:
        AdvancedBlending::default(),
        AdvancedBlending { knockout: Knockout::Deep, blend_interior: true, blend_clipped: false, transparency_shapes: false, layer_mask_hides_effects: true, vector_mask_hides_effects: true },
    );
    ser!(BlendRange: BlendRange::FULL, BlendRange { black: [1, 2], white: [3, 4] });
    ser!(BlendIf: BlendIf::default(), BlendIf { ranges: vec![[BlendRange::FULL; 2], [BlendRange { black: [0, 255], white: [255, 0] }, BlendRange::FULL]] });
    for s in [GradientStyle::Linear, GradientStyle::Radial, GradientStyle::Angle, GradientStyle::Reflected, GradientStyle::Diamond] {
        ser!(GradientStyle: s);
    }
    ser!(Gradient: Gradient::default(), gradient_full());
    ser!(Contour: Contour::Linear, custom_contour(), Contour::Custom { name: String::new(), points: vec![], corners: Vec::new() });
    ser!(FxPaint: FxPaint::Color(colors()[1]), FxPaint::Gradient(gradient_full()), FxPaint::Pattern { name: "n".into(), id: "i".into(), scale: 0.1 });
    ser!(FxCommon: FxCommon::new(BlendMode::Multiply, 0.75), fxc(false, BlendMode::HardMix, -0.0));
    ser!(Shadow: shadow2());
    ser!(GlowTechnique: GlowTechnique::Softer, GlowTechnique::Precise);
    ser!(GlowSource: GlowSource::Center, GlowSource::Edge);
    ser!(Glow: glow(FxPaint::Color(Color::WHITE)), glow(FxPaint::Pattern { name: "p".into(), id: "".into(), scale: 1.0 }));
    ser!(StrokePosition: StrokePosition::Outside, StrokePosition::Inside, StrokePosition::Center);
    ser!(StrokeFx:
        StrokeFx { common: fxc(true, BlendMode::Normal, 1.0), size: 3.0, position: StrokePosition::Outside, paint: FxPaint::Color(Color::BLACK) },
        StrokeFx { common: fxc(true, BlendMode::Normal, 1.0), size: 0.5, position: StrokePosition::Inside, paint: FxPaint::Gradient(Gradient::default()) },
    );
    ser!(Satin: Satin { common: fxc(true, BlendMode::Multiply, 0.5), color: Color::BLACK, angle: 19.0, distance: 11.0, size: 14.0, contour: Contour::Linear, anti_alias: false, invert: true });
    for s in [BevelStyle::OuterBevel, BevelStyle::InnerBevel, BevelStyle::Emboss, BevelStyle::PillowEmboss, BevelStyle::StrokeEmboss] {
        ser!(BevelStyle: s);
    }
    ser!(BevelTechnique: BevelTechnique::Smooth, BevelTechnique::ChiselHard, BevelTechnique::ChiselSoft);
    ser!(Bevel: bevel(true), bevel(false));
    ser!(BevelContour: BevelContour::default(), BevelContour { contour: custom_contour(), range: 1.0, anti_alias: true });
    ser!(BevelTexture: bevel_texture());
    for e in effects_all() {
        ser!(Effect: e);
    }
    ser!(GlobalLight: GlobalLight::default(), GlobalLight { angle: -0.0, altitude: 90.0 });
    ser!(ColorTable: ColorTable::default(), ColorTable { colors: vec![[0, 0, 0], [255, 128, 1]], transparent: Some(1) }, ColorTable { colors: vec![[9, 9, 9]], transparent: Some(255) });
    ser!(DuotoneInk: DuotoneInk::new("Black", [0.0; 3]), DuotoneInk { name: ESC.into(), color: [0.1, 0.5, 1.0], curve: vec![] });
    ser!(Duotone:
        Duotone { inks: vec![], psd_raw: None },
        Duotone { inks: vec![DuotoneInk::new("Black", [0.0; 3]), DuotoneInk::new("PANTONE 151 C", [1.0, 0.5, 0.0])], psd_raw: Some(vec![]) },
        Duotone { inks: vec![], psd_raw: Some(vec![1, 2, 255]) },
    );
    for m in StackMode::ALL {
        ser!(StackMode: m);
    }
    for k in knots() {
        ser!(Knot: k);
    }
    ser!(PathOp: PathOp::Combine, PathOp::Subtract, PathOp::Intersect, PathOp::Exclude, PathOp::Join);
    ser!(FillRule: FillRule::NonZero, FillRule::EvenOdd);
    ser!(Subpath: Subpath::default(), Subpath::polygon(&[(0.0, 0.0), (3.0, 4.0)]).with_op(PathOp::Exclude), Subpath::polyline(&[(0.1, 0.2)]));
    ser!(Path: Path::default(), path_full());
    ser!(ClippingPath: ClippingPath::default(), ClippingPath { name: ESC.into(), flatness: 0.2 });
    ser!(VectorMask: VectorMask::new(Path::default()), VectorMask { path: path_full(), enabled: false, linked: false, density: 0.5, feather: 2.5 });
    ser!(StrokeAlign: StrokeAlign::Inside, StrokeAlign::Center, StrokeAlign::Outside);
    ser!(LineCap: LineCap::Butt, LineCap::Round, LineCap::Square);
    ser!(LineJoin: LineJoin::Miter, LineJoin::Round, LineJoin::Bevel);
    ser!(ShapeStroke:
        ShapeStroke::default(),
        ShapeStroke { width: 0.1, paint: fills()[4].clone(), opacity: 0.5, align: StrokeAlign::Outside, cap: LineCap::Square, join: LineJoin::Bevel, miter_limit: 1e-7, dashes: vec![2.0, 1.0, 0.1], dash_offset: -0.0 },
    );
    ser!(LiveShape:
        LiveShape::Rect { rect: [0.0, 1.0, 2.5, 1e20], radii: [0.1, -0.0, 5e-324, 4.0] },
        LiveShape::Ellipse { rect: [10.0, 10.0, 20.0, 30.0] },
        LiveShape::Polygon { rect: [0.0; 4], sides: u32::MAX, star_ratio: 0.5 },
        LiveShape::Line { from: [0.0, 0.0], to: [-1.0, 1e-7], weight: 2.0 },
    );
    ser!(Kerning: Kerning::Metrics, Kerning::Optical, Kerning::Off);
    ser!(Caps: Caps::Normal, Caps::SmallCaps, Caps::AllCaps);
    ser!(FontFeature: FontFeature { tag: "ss01".into(), value: 65535 }, FontFeature { tag: ESC.into(), value: 0 });
    ser!(FontVariation: FontVariation { axis: "wght".into(), value: 0.1 });
    ser!(CharStyle: CharStyle::default(), char_full(), CharStyle { postscript_name: Some(String::new()), language: Some(ESC.into()), style_sheet: Some(0), ..Default::default() });
    ser!(TextRun: TextRun { len: 0, style: CharStyle::default() }, TextRun { len: usize::MAX, style: char_full() });
    for a in [TextAlign::Left, TextAlign::Center, TextAlign::Right, TextAlign::JustifyLeft, TextAlign::JustifyCenter, TextAlign::JustifyRight, TextAlign::JustifyAll] {
        ser!(TextAlign: a);
    }
    ser!(TextDirection: TextDirection::Auto, TextDirection::Ltr, TextDirection::Rtl);
    ser!(ParagraphStyle: ParagraphStyle::default(), para_full());
    ser!(ParagraphRun: ParagraphRun { len: 9007199254740993, style: para_full() });
    ser!(TextShape: TextShape::Point, TextShape::Box { x: 0.1, y: -0.0, width: 1e20, height: 1e-45 });
    ser!(Orientation: Orientation::Horizontal, Orientation::Vertical);
    for a in [AntiAlias::None, AntiAlias::Sharp, AntiAlias::Crisp, AntiAlias::Strong, AntiAlias::Smooth, AntiAlias::Windows, AntiAlias::WindowsLcd] {
        ser!(AntiAlias: a);
    }
    ser!(TextWarp: TextWarp::default(), TextWarp { style: "warpArc".into(), value: 50.0, horizontal_distortion: -0.1, vertical_distortion: 1e-7, horizontal: true });
    ser!(CharacterStyleDef: CharacterStyleDef::default(), styles_full().character[0].clone());
    ser!(ParagraphStyleDef: ParagraphStyleDef::default(), styles_full().paragraph[0].clone());
    ser!(TextStyles: TextStyles::default(), styles_full());
    ser!(ArtboardBackground: ArtboardBackground::White, ArtboardBackground::Black, ArtboardBackground::Transparent, ArtboardBackground::Custom(colors()[2]));
    ser!(Artboard: Artboard::new(Rect::new(0, 0, 1080, 1920)), Artboard { rect: Rect::new(-1, -2, 3, 4), background: ArtboardBackground::Custom(Color::rgba(0.1, 0.2, 0.3, 0.0)), preset: "iPhone 8/7/6".into() });
    ser!(SliceOrigin: SliceOrigin::Auto, SliceOrigin::Layer, SliceOrigin::User);
    ser!(SliceKind: SliceKind::Image, SliceKind::NoImage, SliceKind::Table);
    ser!(Slice: Slice::default(), slice_full());
    ser!(Slices: Slices::default(), Slices { group_name: "Web".into(), list: vec![slice_full(), Slice { id: 1, ..Default::default() }] });
    ser!(Variables: Variables::default(), variables_full());
    for d in variables_full().defs {
        ser!(VariableDef: d.clone());
        ser!(VarKind: d.kind);
    }
    for m in [PixelMethod::Fit, PixelMethod::Fill, PixelMethod::AsIs, PixelMethod::Conform] {
        ser!(PixelMethod: m);
    }
    for a in [
        PixelAlign::TopLeft, PixelAlign::TopCenter, PixelAlign::TopRight, PixelAlign::CenterLeft, PixelAlign::Center,
        PixelAlign::CenterRight, PixelAlign::BottomLeft, PixelAlign::BottomCenter, PixelAlign::BottomRight,
    ] {
        ser!(PixelAlign: a);
    }
    for s in variables_full().data_sets {
        ser!(DataSet: s.clone());
        for v in s.values {
            ser!(DataValue: v);
        }
    }
    ser!(VideoSource: VideoSource::Blank, VideoSource::File { path: ESC.into() });
    ser!(Timeline: Timeline::new(48, 25.0), Timeline { fps: 29.97, duration: usize::MAX, current: 9007199254740993, work_start: 1, work_end: 0 });
    ser!(MeasurementScale: MeasurementScale::default(), measurement_full().scale);
    ser!(CountGroup: CountGroup::default(), measurement_full().count_groups[1].clone());
    ser!(Ruler: Ruler { start: [0.0, 0.0], end: [1.0, 1.0], protractor: None }, measurement_full().ruler.unwrap());
    ser!(Measurement: Measurement::default(), measurement_full());
    ser!(Note: Note::default(), note_full());

    // Pretty layout.
    serp!(Adjustment: Adjustment::identity_levels(), Adjustment::Invert, adjustments()[4].clone());
    serp!(Slice: Slice::default());
    serp!(Variables: variables_full(), Variables::default());
    serp!(CharacterStyleDef: CharacterStyleDef::default());
    serp!(TextStyles: styles_full());
    serp!(Path: path_full());
    serp!(Effect: effects_all()[9].clone());
    serp!(LiveShape: LiveShape::Line { from: [0.0, 0.0], to: [-1.0, 1e-7], weight: 2.0 });
    serp!(Fill: fills()[2].clone());
    serp!(Measurement: measurement_full());

    // to_value (f32 widened to f64).
    serv!(CharStyle: CharStyle::default(), char_full());
    serv!(ParagraphStyle: ParagraphStyle::default(), para_full());
}

fn de_lines() {
    de!(AdvancedBlending:
        r#"{}"#, r#"{"knockout":"deep"}"#, r#"{"knockout":"Deep"}"#, r#"{"blendClipped":false,"extra":1}"#,
        r#"{"knockout":"deep","knockout":"shallow"}"#, r#"{"blendInterior":1}"#, r#"null"#, r#"[]"#, r#"["deep",true]"#,
        r#"{"blend_interior":true}"#, r#"{"knockout":null}"#, r#"{"knockout":{"deep":null}}"#);
    de!(Size: r#"{"width":1}"#, r#"{"width":-1,"height":0}"#, r#"{"width":4294967296,"height":0}"#, r#"{"width":4294967295,"height":0}"#,
        r#"{"width":1.0,"height":0}"#, r#"{"width":"1","height":0}"#, r#"{"height":2,"width":3,"depth":4}"#, r#"[5,6]"#, r#"[5]"#, r#"[5,6,7]"#,
        r#"{"width":1e0,"height":0}"#, r#"{"width":null,"height":0}"#, r#"{"width":true,"height":0}"#, r#""x""#);
    de!(Rect: r#"{"x0":2147483648,"y0":0,"x1":0,"y1":0}"#, r#"{"x0":-2147483649,"y0":0,"x1":0,"y1":0}"#,
        r#"{"x0":-2147483648,"y0":2147483647,"x1":0,"y1":-0}"#, r#"{"x0":0,"y0":0,"x1":0,"y1":-0.0}"#);
    de!(Point: r#"{"x":1,"y":-1}"#, r#"{"x":18446744073709551615,"y":-9223372036854775808}"#, r#"{"x":1.7976931348623157e308,"y":-5e-324}"#,
        r#"{"x":null,"y":0}"#, r#"{"x":0.1}"#);
    de!(Affine: r#"{"m":[1,0,0,1,0,0]}"#, r#"{"m":[1,0,0,1,0]}"#, r#"{"m":[1,0,0,1,0,0,0]}"#, r#"{"m":"x"}"#);
    de!(BlendRange: r#"{"black":[0],"white":[1,2]}"#, r#"{"black":[1,2,3],"white":[1,2]}"#, r#"{"black":[256,0],"white":[1,2]}"#,
        r#"{"black":[-1,0],"white":[1,2]}"#, r#"{"black":[0.5,0],"white":[1,2]}"#, r#"{"black":[255,0],"white":[0,255]}"#, r#"{"black":null,"white":[1,2]}"#);
    de!(BlendIf: r#"{"ranges":[]}"#, r#"{"ranges":[[{"black":[0,0],"white":[255,255]}]]}"#, r#"{}"#);
    de!(CurvePoint: r#"{"input":1e300,"output":0}"#, r#"{"input":-1e300,"output":1e-50}"#, r#"{"input":null,"output":1}"#, r#"{"input":1,"output":2}"#,
        r#"{"input":18446744073709551615,"output":-9223372036854775808}"#, r#"{"input":true,"output":0}"#, r#"{"output":"x","input":"y"}"#,
        r#"[0.5,1]"#, r#"{"input":3.4028235677973366e38,"output":3.4028235e38}"#, r#"{"input":1.401298464324817e-45,"output":7e-46}"#,
        r#"{"input":0.30000000000000004,"output":16777217}"#);
    de!(LevelsChannel: r#"{"in_black":"a","gamma":"b"}"#, r#"{"gamma":1}"#, r#"{"in_black":0,"in_white":1,"gamma":1,"out_black":0}"#);
    de!(FontFeature: r#"{"tag":"liga","value":65536}"#, r#"{"tag":"liga","value":-1}"#, r#"{"tag":"liga","value":65535}"#, r#"{"tag":1,"value":1}"#);
    de!(Timeline: r#"{"fps":24,"duration":10,"current":3,"work_end":10}"#, r#"{"fps":24,"duration":10,"current":3}"#,
        r#"{"fps":24,"duration":-1,"current":3,"work_end":1}"#, r#"{"fps":24,"duration":1.5,"current":3,"work_end":1}"#,
        r#"{"fps":1e300,"duration":18446744073709551615,"current":0,"work_end":0}"#, r#"{"fps":"24","duration":1,"current":0,"work_end":0}"#);
    de!(Slice: r#"{}"#, r#"{"layer":18446744073709551615}"#, r#"{"layer":null}"#, r#"{"background":[1,2,3]}"#, r#"{"background":null}"#,
        r#"{"kind":"noImage","origin":"layer"}"#, r#"{"kind":"NoImage"}"#, r#"{"horizontalAlign":-1}"#, r#"{"groupId":5,"group_id":6}"#,
        r#"{"cellTextIsHtml":true,"cellText":"<p>","outsets":[1,2,3,4]}"#, r#"{"outsets":[1,2,3]}"#, r#"{"layer":-1}"#, r#"{"rect":{"x0":1}}"#,
        r#"{"background":[1,2,3,256]}"#, r#"{"origin":"auto","id":4294967295}"#);
    de!(Slices: r#"{}"#, r#"{"groupName":"g","list":[{}]}"#, r#"{"list":null}"#);
    de!(Adjustment:
        r#""Invert""#, r#"{"Invert":null}"#, r#"{"Invert":{}}"#, r#"{"Invert":[]}"#, r#""Levels""#,
        r#"{"Levels":{"master":{"in_black":0,"in_white":1,"gamma":1,"out_black":0,"out_white":1},"per_channel":[{"in_black":0,"in_white":1,"gamma":1,"out_black":0,"out_white":1},{"in_black":0,"in_white":1,"gamma":1,"out_black":0,"out_white":1},{"in_black":0,"in_white":1,"gamma":1,"out_black":0,"out_white":1}]}}"#,
        r#"{"Levels":{"master":{"in_black":"x"},"per_channel":[]}}"#,
        r#"{"HueSaturation":{"hue":1,"saturation":2,"lightness":3,"colorize":false}}"#,
        r#"{"Curves":{"master":[],"per_channel":[[],[],[]]}}"#,
        r#"{"Curves":{"master":[],"per_channel":[[],[]]}}"#,
        r#"{"GradientMap":{"stops":[],"reverse":true}}"#,
        r#"{"GradientMap":{"stops":[[0.5,[1,0,0]]],"reverse":true,"dither":true}}"#,
        r#"{"GradientMap":{"stops":[[0.5,[1,0]]],"reverse":true}}"#,
        r#"{"Bogus":{}}"#, r#"{"Posterize":{"levels":-1}}"#, r#"{"Posterize":{"levels":4294967296}}"#, r#"{"Posterize":{"levels":4294967295}}"#,
        r#"{"Posterize":{"levels":2.0}}"#,
        r#"{"ColorLookup":{"name":"x","lut":null,"size":2}}"#, r#"{"ColorLookup":{"name":"x","size":2}}"#,
        r#"{"ColorLookup":{"name":"x","lut":[1,0.5,1e300],"size":2,"tetrahedral":true}}"#,
        r#"{"Invert":null,"Levels":1}"#, r#"{}"#, r#"[]"#, r#"null"#, r#"5"#,
        r#"{"SelectiveColor":{"relative":true,"adjustments":[[0,0,0,0]]}}"#,
        r#"{"ChannelMixer":{"matrix":[[1,0,0,0],[0,1,0,0],[0,0,1,0]],"monochrome":false}}"#,
        r#"{"ChannelMixer":{"matrix":[[1,0,0],[0,1,0,0],[0,0,1,0]],"monochrome":false}}"#,
        r#"{"Unsupported":{"psd_key":"abcd","raw":[0,255,256]}}"#, r#"{"Unsupported":{"psd_key":"abcd","raw":[]}}"#,
        r#"{"BlackWhite":{"weights":[1,2,3,4,5,6],"tint":null}}"#, r#"{"BlackWhite":{"weights":[1,2,3,4,5,6]}}"#,
        r#"{"BrightnessContrast":{"brightness":1,"contrast":2,"legacy":"no"}}"#,
        r#"{"Threshold":{"level":0.5,"level":0.25}}"#);
    de!(LiveShape:
        r#"{"kind":"rect","rect":[0,0,1,1],"radii":[0,0,0,0]}"#, r#"{"kind":"Rect","rect":[0,0,1,1],"radii":[0,0,0,0]}"#,
        r#"{"rect":[0,0,1,1],"radii":[0,0,0,0]}"#, r#"{"kind":"polygon","rect":[0,0,1,1],"sides":5,"star_ratio":0.5}"#,
        r#"{"kind":"line","from":[0,0],"to":[1,1],"weight":2,"extra":1}"#, r#"{"rect":[1,2,3,4],"kind":"ellipse"}"#, r#"{"kind":"ellipse"}"#,
        r#"{"kind":5}"#, r#"{"kind":"polygon","rect":[0,0,1,1],"sides":-5,"star_ratio":0.5}"#, r#""rect""#, r#"{"kind":"ellipse","rect":[0,0,1]}"#,
        r#"{"kind":"line","kind":"ellipse","rect":[0,0,1,1]}"#);
    de!(VarKind: r#"{"type":"visibility"}"#, r#"{"type":"pixelReplacement"}"#,
        r#"{"type":"pixelReplacement","method":"asIs","align":"bottomRight","clip":true}"#, r#"{"type":"textReplacement","x":1}"#,
        r#"{"type":"Visibility"}"#, r#"{}"#, r#""visibility""#, r#"{"type":"pixelReplacement","method":"AsIs"}"#, r#"{"type":"pixelReplacement","clip":null}"#);
    de!(DataValue:
        r#"{"variable":"a","kind":"visibility","value":true}"#, r#"{"variable":"a","kind":"text","value":"x"}"#,
        r#"{"variable":"a","kind":"pixels","value":"/p.png"}"#, r#"{"variable":"a","kind":"text"}"#,
        r#"{"variable":"a","kind":"bogus","value":1}"#, r#"{"variable":"a"}"#, r#"{"kind":"text","value":"x"}"#,
        r#"{"variable":"a","kind":"visibility","value":"yes"}"#, r#"{"variable":"a","kind":"text","value":"x","extra":[1]}"#,
        r#"{"value":"x","variable":"a","kind":"text"}"#, r#"{"variable":1,"kind":"text","value":"x"}"#, r#"{"variable":"a","value":"x"}"#);
    de!(DataSet: r#"{"name":"s"}"#, r#"{"values":[]}"#, r#"{"name":"s","values":[{"variable":"v","kind":"visibility","value":false}]}"#);
    de!(Variables: r#"{}"#, r#"{"active":null}"#, r#"{"active":-1}"#, r#"{"active":18446744073709551615}"#,
        r#"{"defs":[{"name":"a","layer":1,"kind":{"type":"visibility"}}]}"#, r#"{"defs":[{"name":"a","layer":18446744073709551615,"kind":{"type":"visibility"}}]}"#,
        r#"{"defs":[{"name":"a","layer":-1,"kind":{"type":"visibility"}}]}"#, r#"{"defs":[{"name":"a","layer":1.5,"kind":{"type":"visibility"}}]}"#,
        r#"{"defs":[{"name":"a","kind":{"type":"visibility"}}]}"#, r#"{"data_sets":[{"name":"x"}],"defs":null}"#);
    de!(ShapeStroke: r#"{}"#, r#"{"width":"x"}"#, r#"{"paint":{"Pattern":{"name":"p","scale":1}}}"#, r#"{"dashes":[1,2],"cap":"Round"}"#, r#"{"cap":"round"}"#);
    de!(Fill: r#"{"Pattern":{"name":"p","scale":1}}"#, r#"{"Gradient":{"stops":[],"angle":0,"scale":1,"style":"Linear","reverse":false}}"#,
        r#"{"Solid":{"mode":"Rgb","c":[1,0,0,0],"alpha":1}}"#, r#"{"Solid":null}"#, r#"{"Solid":{"mode":"Rgb","c":[1,0,0],"alpha":1}}"#,
        r#"{"Gradient":{"stops":[[0,{"mode":"Rgb","c":[1,0,0,0],"alpha":1}]],"angle":0,"scale":1,"style":"Linear","reverse":false,"align":false,"midpoints":[0.25]}}"#,
        r#"{"Pattern":{"scale":1}}"#, r#""Solid""#);
    de!(Effect:
        r#"{"PatternOverlay":{"common":{"enabled":true,"blend":"Normal","opacity":1},"name":"n","id":"i","scale":1}}"#,
        r#"{"ColorOverlay":{"common":{"enabled":true,"blend":"Normal","opacity":1},"color":{"mode":"Rgb","c":[1,0,0,0],"alpha":1}}}"#,
        r#"{"ColorOverlay":{"common":{"enabled":true,"blend":"normal","opacity":1},"color":{"mode":"Rgb","c":[1,0,0,0],"alpha":1}}}"#,
        r#"{"Satin":null}"#, r#"{"Bogus":null}"#);
    de!(Bevel: r#"{"enabled":true,"style":"Emboss","technique":"Smooth","depth":1,"up":true,"size":5,"soften":0,"angle":120,"altitude":30,"use_global_light":true,"gloss_contour":"Linear","highlight":{"enabled":true,"blend":"Screen","opacity":0.75},"highlight_color":{"mode":"Rgb","c":[1,1,1,0],"alpha":1},"shadow":{"enabled":true,"blend":"Multiply","opacity":0.75},"shadow_color":{"mode":"Rgb","c":[0,0,0,0],"alpha":1}}"#,
        r#"{"enabled":true,"style":"Emboss","technique":"Smooth","depth":1,"up":true,"size":5,"soften":0,"angle":120,"altitude":30,"use_global_light":true,"gloss_contour":{"Custom":{"name":"c","points":[]}},"highlight":{"enabled":true,"blend":"Screen","opacity":0.75},"highlight_color":{"mode":"Rgb","c":[1,1,1,0],"alpha":1},"shadow":{"enabled":true,"blend":"Multiply","opacity":0.75},"shadow_color":{"mode":"Rgb","c":[0,0,0,0],"alpha":1},"contour":{"contour":"Linear","range":0.5,"anti_alias":false},"texture":null}"#);
    de!(Contour: r#""Linear""#, r#"{"Linear":null}"#, r#"{"Custom":{"name":"x"}}"#, r#""Custom""#, r#"{"Custom":{"name":"x","points":[{"input":0,"output":1}]}}"#);
    de!(CharStyle: r#"{}"#, r#"{"size_pt":1e300}"#, r#"{"weight":65536}"#, r#"{"leading_pt":null}"#, r#"{"leading_pt":12}"#, r#"{"style_sheet":4294967295}"#,
        r#"{"style_sheet":null}"#, r#"{"style_sheet":4294967296}"#, r#"{"features":[{"tag":"liga"}]}"#, r#"{"kerning":"optical"}"#,
        r#"{"caps":"AllCaps","ligatures":false}"#, r#"{"color":{"mode":"Lab","c":[0.5,0.5,0.5,0],"alpha":0.5},"unknown":[1,2]}"#,
        r#"{"postscript_name":"PS","language":null,"variations":[{"axis":"wdth","value":75}]}"#, r#"{"font_family":null}"#, r#"[]"#);
    de!(ParagraphStyle: r#"{"align":"Center"}"#, r#"{"auto_leading":null}"#, r#"{"direction":"Rtl","style_sheet":0}"#, r#"{"align":"Bogus"}"#, r#"{}"#);
    de!(TextShape: r#""Point""#, r#"{"Box":{"x":1,"y":2,"width":3,"height":4}}"#, r#"{"Box":{"x":1}}"#, r#"{"Point":null}"#, r#""Box""#);
    de!(TextRun: r#"{"len":-1,"style":{}}"#, r#"{"len":18446744073709551615,"style":{}}"#, r#"{"len":3}"#);
    de!(TextStyles: r#"{}"#, r#"{"character":[{"id":1}]}"#, r#"{"basic":{}}"#, r#"{"psd_raw":{"b":1,"a":[2,{"d":0,"c":1.5}]}}"#, r#"{"psd_raw":null}"#,
        r#"{"paragraph":[{"id":3,"name":"P","para_attrs":{"align":"Right"},"char_attrs":{}}]}"#);
    de!(CharacterStyleDef: r#"{"id":-1}"#, r#"{"attrs":[]}"#, r#"{"attrs":{"z":1,"a":{"y":2,"x":3},"m":1.0,"n":-0.0}}"#, r#"{"id":4294967295,"name":"x"}"#);
    de!(Measurement: r#"{}"#, r#"{"scale":{"units":"cm"}}"#, r#"{"ruler":{"start":[0,0],"end":[1,1]}}"#, r#"{"ruler":{"start":[0,0]}}"#,
        r#"{"active_count_group":-1}"#, r#"{"count_groups":[{}]}"#, r#"{"ruler":null}"#, r#"{"ruler":{"start":[0,0],"end":[1,1],"protractor":[2,2]}}"#);
    de!(Note: r#"{"text":"hi"}"#, r#"{"position":[1]}"#, r#"{"popup":[1,2,3,4,5]}"#, r#"{"open":"yes"}"#);
    de!(MeasurementScale: r#"{"pixel_length":"1"}"#, r#"{"pixel_length":2}"#, r#"{"logical_length":1e308}"#);
    de!(CountGroup: r#"{"points":[[1,2],[3]]}"#, r#"{"marker_size":-1}"#, r#"{"name":"G"}"#);
    de!(Knot: r#"{"anchor":{"x":0,"y":0},"in_ctrl":{"x":0,"y":0},"out_ctrl":{"x":1,"y":1}}"#, r#"{"anchor":{"x":0,"y":0},"in_ctrl":{"x":0,"y":0}}"#,
        r#"{"anchor":{"x":0,"y":0},"in_ctrl":{"x":0,"y":0},"out_ctrl":{"x":1,"y":1},"smooth":null}"#);
    de!(Subpath: r#"{"closed":true,"knots":[]}"#, r#"{"knots":[]}"#, r#"{"closed":true,"knots":[],"op":"Join"}"#, r#"{"closed":true,"knots":[],"op":"join"}"#);
    de!(Path: r#"{"subpaths":[]}"#, r#"{}"#, r#"{"subpaths":[{"closed":true,"knots":[]}],"fill_rule":"EvenOdd","inverted":true}"#);
    de!(VectorMask: r#"{"path":{"subpaths":[]},"enabled":true,"linked":true,"density":1}"#);
    de!(Color: r#"{"mode":"Rgb","c":[0,0,0,0]}"#, r#"{"mode":"RGB","c":[0,0,0,0],"alpha":1}"#, r#"{"mode":"Rgb","c":[0,0,0,0,0],"alpha":1}"#,
        r#"{"mode":"Cmyk","c":[0.1,0.2,0.3,0.4],"alpha":0.5}"#);
    de!(BlendMode: r#""Normal""#, r#""normal""#, r#"{"Normal":null}"#, r#"{"Dissolve":1}"#);
    de!(LabelColor: r#""Seafoam""#, r#""Unknown""#, r#""seafoam""#);
    de!(SampleType: r#""U16""#, r#""u16""#);
    de!(PixelFormat: r#"{"mode":"Rgb","sample":"U8","alpha":1}"#, r#"{"mode":"Rgb","sample":"U8","alpha":false}"#);
    de!(Locks: r#"{}"#, r#"{"transparency":null,"pixels":false,"position":false,"artboard":false,"all":false}"#,
        r#"{"transparency":true,"pixels":false,"position":false,"artboard":false,"all":false}"#);
    de!(Guides: r#"{"horizontal":[1],"vertical":null}"#, r#"{"horizontal":[1,2.5],"vertical":[]}"#);
    de!(ColorTable: r#"{"colors":[[0,0,0]]}"#, r#"{"colors":[],"transparent":256}"#, r#"{"colors":[[0,0]]}"#, r#"{"colors":[],"transparent":null}"#);
    de!(Duotone: r#"{"inks":[]}"#, r#"{"inks":[],"psd_raw":[1,2]}"#, r#"{"inks":[],"psd_raw":[]}"#, r#"{"inks":[],"psd_raw":null}"#,
        r#"{"inks":[{"name":"k","color":[0,0,0],"curve":[]}],"psd_raw":[-1]}"#);
    de!(Artboard: r#"{"rect":{"x0":0,"y0":0,"x1":1,"y1":1},"background":"white"}"#, r#"{"rect":{"x0":0,"y0":0,"x1":1,"y1":1},"background":"White"}"#,
        r#"{"rect":{"x0":0,"y0":0,"x1":1,"y1":1},"background":{"custom":{"mode":"Rgb","c":[1,0,0,0],"alpha":1}},"preset":"p"}"#);
    de!(ArtboardBackground: r#""transparent""#, r#""custom""#, r#"{"custom":null}"#, r#"{"black":null}"#);
    de!(SliceKind: r#""table""#, r#""img""#);
    de!(SliceOrigin: r#""auto""#, r#""User""#);
    de!(SmartFilter: r#"{"command":"x","params":{"b":1,"a":2},"blend":"Normal","opacity":1,"visible":true}"#,
        r#"{"command":"x","blend":"Normal","opacity":1,"visible":true}"#, r#"{"command":"x","params":null,"blend":"Normal","opacity":1,"visible":true}"#,
        r#"{"command":"x","params":[1.0,-0,0.1,1e300,-1],"blend":"Normal","opacity":1,"visible":true}"#);
    de!(GlowSource: r#""Center""#, r#""center""#);
    de!(BevelStyle: r#""InnerBevel""#);
    de!(Shadow: r#"{"common":{"enabled":true,"blend":"Normal","opacity":1},"color":{"mode":"Rgb","c":[0,0,0,0],"alpha":1},"angle":1,"use_global_light":true,"distance":1,"spread":0,"size":1,"contour":"Linear","anti_alias":false,"noise":0}"#);
    de!(HueRange: r#"{"hue":0,"saturation":0,"lightness":0,"bounds":[1,2,3]}"#, r#"{"hue":0,"saturation":0,"lightness":0,"bounds":[1,2,3,4]}"#);
    de!(Kerning: r#"null"#, r#""Off""#);
    de!(VideoSource: r#""Blank""#, r#"{"File":{"path":"/v.mp4"}}"#, r#"{"File":{}}"#, r#""File""#);
    de!(Gradient: r#"{"stops":[],"opacity_stops":[],"style":"Linear","angle":0,"scale":1,"reverse":false,"align":true,"offset":[1]}"#,
        r#"{"stops":[],"opacity_stops":[],"style":"Linear","angle":0,"scale":1,"reverse":false,"align":true,"offset":[1,2,3]}"#,
        r#"{"stops":[],"opacity_stops":[[0.5,1]],"style":"Linear","angle":0,"scale":1,"reverse":false,"align":true,"offset":"x"}"#,
        r#"{"stops":[[0.5]],"opacity_stops":[],"style":"Linear","angle":0,"scale":1,"reverse":false,"align":true,"offset":[0,0]}"#);
    de!(TextWarp: r#"{"style":"warpArc","value":1,"horizontal_distortion":0,"vertical_distortion":0,"horizontal":true}"#, r#"{}"#);
    de!(ClippingPath: r#"{"name":"c","flatness":0.2}"#, r#"{"name":"c"}"#);
    de!(GlobalLight: r#"{"angle":120,"altitude":30}"#, r#"{"angle":120}"#);
    de!(StackMode: r#""StandardDeviation""#, r#""standardDeviation""#);
}

// ---------------------------------------------------------------------------------------------
// Logic vectors

fn sweep_degrees() -> Vec<f32> {
    let mut v: Vec<f32> = (-1440..=1440).map(|i| i as f32 * 0.5).collect();
    v.extend([f32::NAN, f32::INFINITY, f32::NEG_INFINITY, -0.0, 1e-7, 359.99997, 1e20, -1e20, sub1()]);
    v
}

fn hue_fns() {
    for i in 0..6 {
        l!("hue_neutral", i, js!(&HueRange::neutral(i)));
    }
    l!("hue_defaults", js!(&HueRange::defaults()));
    let bounds: Vec<[f32; 4]> = vec![
        HueRange::neutral(0).bounds,
        HueRange::neutral(1).bounds,
        HueRange::neutral(2).bounds,
        HueRange::neutral(3).bounds,
        HueRange::neutral(4).bounds,
        HueRange::neutral(5).bounds,
        [10.0, 10.0, 10.0, 10.0],
        [f32::NAN; 4],
        [0.0, f32::NAN, 30.0, 60.0],
        [f32::INFINITY, 0.0, 10.0, 20.0],
        [350.0, 10.0, 5.0, 40.0],
        [-720.0, 0.0, 720.0, 1440.0],
        [100.0, 90.0, 80.0, 70.0],
        [0.0, 0.0, 360.0, 360.0],
        [-0.0, 0.1, 0.2, 0.3],
        [315.0, 345.0, 15.0, 45.0],
        [1e20, -1e20, 3.0, 4.0],
        [0.0, 359.0, 359.5, 360.0],
    ];
    for b in &bounds {
        let r = HueRange { hue: 1.0, saturation: 0.0, lightness: 0.0, bounds: *b };
        let mut h = H0;
        for d in sweep_degrees() {
            fnv(&mut h, &r.weight(d).to_bits().to_le_bytes());
        }
        l!("hue_weight_sweep", xs32(b), format!("{h:016x}"));
        for d in [0.0f32, 30.0, 330.0, 15.5, 44.9, f32::NAN, 720.0, -30.0] {
            l!("hue_weight", xs32(b), x32(d), x32(r.weight(d)));
        }
        l!("canonical_bounds", xs32(b), xs32(&HueRange::canonical_bounds(*b)));
    }
    for b in [[0.0f32, 30.0, 60.0, 90.0], [300.0, -30.0, 30.0, 60.0], [0.0, 400.0, 800.0, 1200.0], [5.0, 4.0, 3.0, 2.0], [-1e-7, 1e-7, 0.5, 359.0], [0.0, 360.0, 720.0, 1080.0], [1e10, 1e10 + 5.0, 1e10, 1e10]] {
        l!("canonical_bounds", xs32(&b), xs32(&HueRange::canonical_bounds(b)));
    }
    for r in [HueRange::neutral(2), HueRange { hue: -0.0, ..HueRange::neutral(0) }, HueRange { lightness: 1e-45, ..HueRange::neutral(0) }] {
        l!("hue_is_neutral", js!(&r), r.is_neutral());
    }
}

fn blend_fns() {
    let ranges = [
        BlendRange::FULL,
        BlendRange { black: [50, 50], white: [200, 200] },
        BlendRange { black: [0, 100], white: [155, 255] },
        BlendRange { black: [100, 0], white: [255, 155] },
        BlendRange { black: [255, 255], white: [0, 0] },
        BlendRange { black: [0, 255], white: [0, 255] },
        BlendRange { black: [10, 20], white: [230, 240] },
    ];
    for r in ranges {
        let mut h = H0;
        let mut vals: Vec<f32> = (-40..=1060).map(|i| i as f32 * 0.25).collect();
        vals.extend([f32::NAN, f32::INFINITY, f32::NEG_INFINITY, 1e-7, 254.99998]);
        for v in vals {
            fnv(&mut h, &r.weight(v).to_bits().to_le_bytes());
        }
        l!("blend_weight_sweep", js!(&r), format!("{h:016x}"), r.is_full(), js!(&r.to_bytes()), js!(&BlendRange::from_bytes(r.to_bytes())));
        for v in [0.0f32, 49.9, 50.0, 127.5, 200.1, 255.0, f32::NAN] {
            l!("blend_weight", js!(&r), x32(v), x32(r.weight(v)));
        }
    }
    // BlendIf::set sequences: "i:bl,bh,wl,wh,bl,bh,wl,wh;..."
    let seqs = [
        "0:0,0,255,255,0,0,255,255",
        "2:1,2,3,4,0,0,255,255",
        "2:1,2,3,4,0,0,255,255;2:0,0,255,255,0,0,255,255",
        "1:0,0,255,255,5,6,7,8;3:0,0,255,255,0,0,255,255",
        "0:9,9,9,9,9,9,9,9;4:0,0,255,255,1,1,255,255;4:0,0,255,255,0,0,255,255",
        "3:0,0,255,255,0,0,255,254;0:0,0,255,255,0,0,255,255;3:0,0,255,255,0,0,255,255",
    ];
    for s in seqs {
        let mut b = BlendIf::default();
        for op in s.split(';') {
            let (i, rest) = op.split_once(':').unwrap();
            let v: Vec<u8> = rest.split(',').map(|x| x.parse().unwrap()).collect();
            b.set(i.parse().unwrap(), [BlendRange { black: [v[0], v[1]], white: [v[2], v[3]] }, BlendRange { black: [v[4], v[5]], white: [v[6], v[7]] }]);
        }
        let gets: Vec<[BlendRange; 2]> = (0..6).map(|i| b.get(i)).collect();
        l!("blend_if_set", s, js!(&b), b.is_default(), js!(&gets));
    }
    for b in [BlendIf { ranges: vec![[BlendRange::FULL; 2]; 3] }, BlendIf::default()] {
        l!("blend_if_is_default", js!(&b), b.is_default());
    }
    for k in [Knockout::None, Knockout::Shallow, Knockout::Deep] {
        l!("knockout", js!(&k), k.to_psd(), k.name());
    }
    for v in [0u8, 1, 2, 3, 7, 255] {
        l!("knockout_from_psd", v, js!(&Knockout::from_psd(v)));
    }
    for s in ["none", "shallow", "deep", "DEEP", "Shallow", "nOnE", "medium", "", "deep ", "ＤＥＥＰ"] {
        l!("knockout_from_name", s, Knockout::from_name(s).map_or("None".into(), |k| js!(&k)));
    }
    let a = AdvancedBlending::default();
    l!("advanced_default", js!(&a), a.is_default());
    let b = AdvancedBlending { blend_clipped: false, ..a };
    l!("advanced_is_default", js!(&b), b.is_default());
}

fn adjust_fns() {
    for s in [0u32, 1, 2, 3, 33, 64, 256, 1 << 20, (1 << 21) - 1, 1 << 21, 2642245, 2642246, 1 << 22, u32::MAX] {
        l!("lut3d_len", s, opt(lut3d_len(s)));
    }
    let curves: Vec<Vec<CurvePoint>> = vec![
        vec![],
        curve(&[(0.5, 0.5)]),
        curve(&[(0.0, 0.0), (1.0, 1.0)]),
        curve(&[(-0.0, 0.0), (1.0, 1.0)]),
        curve(&[(0.0, 0.0), (1.0, 0.999)]),
        curve(&[(0.0, 0.0), (0.5, 0.5), (1.0, 1.0)]),
        curve(&[(1.0, 1.0), (0.0, 0.0)]),
    ];
    for c in &curves {
        l!("is_identity_curve", js!(c), is_identity_curve(c));
    }
    for a in adjustments() {
        l!("adj_label", js!(&a), a.label());
    }
    l!("adj_default", "hue_saturation", js!(&Adjustment::default_hue_saturation()));
    l!("adj_default", "identity_curves", js!(&Adjustment::identity_curves()));
    l!("adj_default", "identity_levels", js!(&Adjustment::identity_levels()));
    l!("default", "LevelsChannel", js!(&LevelsChannel::default()));
}

fn mode_fns() {
    let tables = [
        ColorTable::default(),
        ColorTable { colors: vec![[0, 0, 0], [255, 255, 255], [255, 0, 0]], transparent: None },
        ColorTable { colors: vec![[10, 10, 10], [10, 10, 10], [0, 0, 1], [0, 0, 0]], transparent: Some(3) },
    ];
    let cs: [[f32; 3]; 7] = [[0.9, 0.1, 0.1], [0.6, 0.6, 0.6], [0.0; 3], [f32::NAN, 0.0, 0.0], [1e20, 0.0, 0.0], [0.0392157, 0.0392157, 0.0392157], [-0.0, 0.002, 0.0]];
    for t in &tables {
        for c in cs {
            l!("ct_nearest", js!(t), xs32(&c), t.nearest(c));
        }
        for i in [0usize, 1, 2, 3, 4, usize::MAX] {
            l!("ct_rgb", js!(t), i, xs32(&t.rgb(i)));
        }
    }
    let inks = [
        DuotoneInk::new("Black", [0.0; 3]),
        DuotoneInk { name: "c".into(), color: [1.0, 0.5, 0.0], curve: vec![] },
        DuotoneInk { name: "c".into(), color: [1.0, 0.5, 0.0], curve: curve(&[(0.3, 0.7)]) },
        DuotoneInk { name: "c".into(), color: [0.2, 0.4, 0.6], curve: curve(&[(0.0, 0.1), (0.5, 0.9), (0.5, 0.2), (1.0, 2.0)]) },
        DuotoneInk { name: "c".into(), color: [0.0; 3], curve: curve(&[(0.2, 0.0), (0.2000001, 1.0), (0.8, -1.0)]) },
        DuotoneInk { name: "c".into(), color: [0.0; 3], curve: curve(&[(sub1(), 0.5), (sub1(), 0.75), (1.0, 1.0)]) },
    ];
    let xs: Vec<f32> = {
        let mut v: Vec<f32> = (-10..=110).map(|i| i as f32 / 100.0).collect();
        v.extend([f32::NAN, f32::INFINITY, -0.0, 0.2000001, 1e-7]);
        v
    };
    for ink in &inks {
        let mut h = H0;
        for x in &xs {
            fnv(&mut h, &ink.density(*x).to_bits().to_le_bytes());
        }
        l!("ink_density_sweep", js!(ink), format!("{h:016x}"));
        for x in [0.0f32, 0.25, 0.5, 1.0, f32::NAN] {
            l!("ink_density", js!(ink), x32(x), x32(ink.density(x)));
        }
    }
    let duos = [
        Duotone { inks: vec![], psd_raw: None },
        Duotone { inks: vec![inks[0].clone()], psd_raw: None },
        Duotone { inks: vec![inks[0].clone(), DuotoneInk::new("Orange", [1.0, 0.5, 0.0])], psd_raw: None },
        Duotone { inks: inks.to_vec(), psd_raw: Some(vec![1]) },
    ];
    for d in &duos {
        let mut h = H0;
        for x in &xs {
            for v in d.render(*x) {
                fnv(&mut h, &v.to_bits().to_le_bytes());
            }
        }
        l!("duotone_render_sweep", js!(d), format!("{h:016x}"));
        for g in [0.0f32, 0.25, 0.5, 1.0, f32::NAN] {
            l!("duotone_render", js!(d), x32(g), xs32(&d.render(g)));
        }
        l!("duotone_display", js!(d), js!(&d.display_adjustment()));
    }
    for (i, m) in StackMode::ALL.into_iter().enumerate() {
        l!("stack_mode", i, m.id(), m.label(), js!(&m), opt(StackMode::from_id(m.id()).map(|x| js!(&x))));
    }
    for s in ["", "Mean", "standarddeviation", "mean ", "variance"] {
        l!("stack_mode_from_id", s, opt(StackMode::from_id(s).map(|x| js!(&x))));
    }
}

fn label_fns() {
    for (i, c) in LabelColor::ALL.into_iter().enumerate() {
        l!("label_color", i, js!(&c), c.id(), c.label(), opt(LabelColor::from_id(c.id()).map(|x| js!(&x))));
    }
    for s in ["", "Red", "0", "unknown", "🔴", "gray ", "grey"] {
        l!("label_from_id", s, opt(LabelColor::from_id(s).map(|x| js!(&x))));
    }
    for c in [0u32, 1, 2, 3, 99, u32::MAX] {
        let o = SliceOrigin::from_code(c);
        let k = SliceKind::from_code(c);
        l!("slice_codes", c, js!(&o), o.code(), o.id(), js!(&k), k.code(), k.id());
    }
    for s in ["image", "img", "noImage", "none", "no-image", "table", "Table", "", "noimage"] {
        l!("slice_kind_from_id", s, opt(SliceKind::from_id(s).map(|x| js!(&x))));
    }
    for e in effects_all() {
        l!("effect_label", js!(&e), e.label(), e.enabled());
    }
    l!("default", "drop_shadow", js!(&Effect::default_drop_shadow()));
    l!("default", "Gradient", js!(&Gradient::default()));
    l!("default", "BevelContour", js!(&BevelContour::default()));
    l!("default", "GlobalLight", js!(&GlobalLight::default()));
    l!("default", "FxCommon", js!(&FxCommon::new(BlendMode::Overlay, 0.25)));
    l!("default", "ShapeStroke", js!(&ShapeStroke::default()));
    l!("default", "CharStyle", js!(&CharStyle::default()));
    l!("default", "ParagraphStyle", js!(&ParagraphStyle::default()));
    l!("default", "TextStyles", js!(&TextStyles::default()));
    l!("default", "MeasurementScale", js!(&MeasurementScale::default()));
    l!("default", "CountGroup", js!(&CountGroup::default()));
    l!("default", "Note", js!(&Note::default()));
    l!("default", "Measurement", js!(&Measurement::default()));
    l!("default", "AdvancedBlending", js!(&AdvancedBlending::default()));
    l!("default", "Slice", js!(&Slice::default()));
    l!("default", "DuotoneInk", js!(&DuotoneInk::new("Ink 1", [0.1, 0.2, 0.3])));
    l!("default", "VectorMask", js!(&VectorMask::new(Path::default())));
    l!("default", "Artboard", js!(&Artboard::new(Rect::new(1, 2, 3, 4))));
    l!("default", "FillGradient", js!(&Fill::gradient(vec![(0.0, Color::BLACK), (0.5, colors()[1])], 45.0, 0.5, GradientStyle::Angle, true)));
    l!("default", "Knots", js!(&knots()));
    for a in [TextAlign::Left, TextAlign::Center, TextAlign::Right, TextAlign::JustifyLeft, TextAlign::JustifyCenter, TextAlign::JustifyRight, TextAlign::JustifyAll] {
        l!("text_align_justified", js!(&a), a.is_justified());
    }
    let contents: Vec<LayerContent> = vec![
        LayerContent::Raster(Surface::new(PixelFormat::RGBA8)),
        LayerContent::Group(photocraft_doc::Group { children: vec![], expanded: true, artboard: None }),
        LayerContent::Adjustment(Adjustment::Invert),
        LayerContent::Fill(Fill::Solid(Color::BLACK)),
        LayerContent::Text(TextLayer::default()),
        LayerContent::Shape(ShapeLayer::default()),
        LayerContent::Smart(SmartObject::new(SmartSource::Linked { path: "x".into() }, Affine::IDENTITY, None)),
    ];
    for (i, c) in contents.iter().enumerate() {
        l!("content_kind", i, c.kind_name(), c.article());
    }
    for bg in [ArtboardBackground::White, ArtboardBackground::Black, ArtboardBackground::Transparent, ArtboardBackground::Custom(Color::rgba(0.1, 0.2, 0.3, 0.4))]
        .into_iter()
        .chain(colors().into_iter().map(ArtboardBackground::Custom))
    {
        l!("artboard_bg", js!(&bg), bg.rgba().map_or("None".into(), |v| xs32(&v)), bg.psd_type(), bg.name());
    }
    for v in [0i32, 1, 2, 3, 4, 5, 6, 7, 8, -1, i32::MIN, 13] {
        let mut c = LayerComp { id: 1, name: "c".into(), comment: String::new(), apply_visibility: false, apply_position: false, apply_appearance: false, states: vec![] };
        c.set_captured_info(v);
        l!("comp_info", v, c.captured_info(), c.apply_visibility, c.apply_position, c.apply_appearance);
    }
    for (spot, sel) in [(false, false), (false, true), (true, false), (true, true)] {
        let mut a = AlphaChannel::new("a", Surface::new(PixelFormat::GRAY8));
        if spot {
            a.spot = Some((Color::rgb(0.0, 1.0, 0.0), 0.5));
        }
        if sel {
            a.indicates = ColorIndicates::SelectedAreas;
        }
        for v in [0.0f32, 0.25, 1.0, -0.0, f32::NAN, 2.0] {
            l!("overlay_coverage", spot, sel, x32(v), x32(a.overlay_coverage(v)));
        }
    }
    let l1 = Locks { transparency: true, ..Default::default() };
    let l2 = Locks { pixels: true, all: true, ..Default::default() };
    l!("locks_union", js!(&l1), js!(&l2), js!(&l1.union(l2)));
}

fn mask_fns() {
    // (reveal?, enabled, density, fill value)
    for (reveal, enabled, density, fill) in [
        (true, true, 1.0f32, None),
        (false, true, 1.0, None),
        (false, true, 0.5, None),
        (false, false, 1.0, None),
        (true, true, 0.25, Some(0.5f32)),
        (false, true, 0.3, Some(0.2)),
        (false, true, 0.0, Some(0.0)),
        (true, true, 1e-7, Some(1.0)),
        (false, true, 2.0, Some(0.1)),
        (false, true, f32::NAN, Some(0.7)),
    ] {
        let mut m = if reveal { LayerMask::reveal_all() } else { LayerMask::hide_all() };
        m.enabled = enabled;
        m.density = density;
        if let Some(v) = fill {
            m.surface.fill_rect(Rect::new(0, 0, 3, 2), &[v]);
        }
        let mut out = Vec::new();
        for (x, y) in [(0, 0), (2, 1), (3, 1), (-1, 0), (1000, 1000)] {
            out.push(x32(m.value(x, y)));
        }
        let mut vals = Vec::new();
        m.values_into(Rect::new(-1, -1, 4, 3), &mut vals);
        l!("mask_value", reveal, enabled, x32(density), fill.map_or("-".into(), x32), out.join(","), xs32(&vals));
    }
}

const AFF: Affine = Affine { m: [2.0, 0.5, -1.0, 3.0, 10.0, -20.0] };

fn path_fns() {
    let paths = vec![
        Path::default(),
        path_full(),
        Path::new(vec![Subpath { closed: true, knots: vec![], op: PathOp::Join }]),
        Path::new(vec![Subpath::polyline(&[(1.0, 2.0)]).with_op(PathOp::Intersect), Subpath::polygon(&[(-5.0, 7.0), (3.0, -1.0)]).with_op(PathOp::Join)]),
        Path::new(vec![Subpath { closed: false, knots: knots(), op: PathOp::Exclude }]),
    ];
    for p in &paths {
        let comps: Vec<String> = p.components().iter().map(|r| format!("{}..{}", r.start, r.end)).collect();
        let ops: Vec<String> = (0..p.subpaths.len()).map(|i| js!(&p.effective_op(i))).collect();
        let cb = p.control_bounds().map_or("None".into(), |b| [x64(b.0), x64(b.1), x64(b.2), x64(b.3)].join(","));
        l!("path", js!(p), comps.join(","), ops.join(","), cb, p.is_empty(), js!(&p.transform(&AFF)));
    }
    let subs = vec![
        Subpath::default(),
        Subpath::polygon(&[(0.0, 0.0)]),
        Subpath::polygon(&[(0.0, 0.0), (10.0, 0.0), (10.0, 10.0)]),
        Subpath::polyline(&[(0.0, 0.0), (10.0, 0.0), (10.0, 10.0)]),
        Subpath { closed: true, knots: knots(), op: PathOp::Combine },
        Subpath { closed: false, knots: knots(), op: PathOp::Combine },
    ];
    for s in &subs {
        l!("subpath_segments", js!(s), s.segments().len(), js!(&s.segments()));
    }
    for pts in [vec![], vec![(0.1, 0.2)], vec![(0.0, 0.0), (1e20, -0.0), (5e-324, 3.0)]] {
        l!("subpath_poly", js!(&pts), js!(&Subpath::polygon(&pts)), js!(&Subpath::polyline(&pts)));
    }
    for k in knots() {
        l!("knot_transform", js!(&k), js!(&k.transform(&AFF)));
    }
}

fn surface_with(format: PixelFormat, rect: Rect, px: &[f32]) -> Surface {
    let mut s = Surface::new(format);
    s.fill_rect(rect, px);
    s
}

fn pattern_fns() {
    let cases: Vec<(&str, PixelFormat, u32, u32, Rect, Vec<f32>)> = vec![
        ("$$$/Patterns/Defaults/Water=Water", PixelFormat::RGBA8, 4, 4, Rect::new(0, 0, 4, 4), vec![1.0, 0.0, 0.0, 1.0]),
        ("Other", PixelFormat::RGBA8, 4, 4, Rect::new(0, 0, 4, 4), vec![1.0, 0.0, 0.0, 1.0]),
        ("Other", PixelFormat::RGBA16, 4, 4, Rect::new(0, 0, 4, 4), vec![1.0, 0.0, 0.0, 1.0]),
        ("Other", PixelFormat::RGBA32F, 4, 4, Rect::new(0, 0, 4, 4), vec![1.0, 0.0, 0.0, 1.0]),
        ("Gray", PixelFormat::GRAY8, 3, 2, Rect::new(1, 0, 2, 2), vec![0.5]),
        ("Cmyk é", PixelFormat::CMYKA8, 2, 3, Rect::new(0, 0, 1, 1), vec![0.1, 0.2, 0.3, 0.4, 0.5]),
        ("Lab", PixelFormat::new(ColorMode::Lab, SampleType::U16, false), 1, 1, Rect::new(0, 0, 1, 1), vec![0.5, 0.25, 0.75]),
        ("", PixelFormat::RGBA8, 0, 0, Rect::new(0, 0, 0, 0), vec![0.0; 4]),
        ("Café\u{0}\u{0}", PixelFormat::GRAYA8, 5, 1, Rect::new(0, 0, 5, 1), vec![0.2, 0.9]),
    ];
    let mut pats = Vec::new();
    for (name, f, w, h, r, px) in cases {
        let p = Pattern::new(name, surface_with(f, r, &px), w, h);
        l!("pattern_new", name.replace('\0', "\\0"), js!(&f), w, h, js!(&r), xs32(&px), &p.id, p.display_name().replace('\0', "\\0"), js!(&p.rect()), p.is_empty());
        pats.push(p);
    }
    for n in ["", "\u{0}", "Water", "$$$/x", "$$$/x=", "$$$/a=b=c", "$$$/=shown\u{0}", "a\u{0}b\u{0}\u{0}", "$$/x=y", "日本\u{0}"] {
        l!("pattern_display_name", n.replace('\0', "\\0"), pattern::display_name(n).replace('\0', "\\0"));
    }
    // Lookups by "#i" (id of pattern i) or a literal id.
    let lookups: Vec<(&str, &str)> = vec![
        ("#0", ""),
        ("#3", "Water"),
        ("nope", "water"),
        ("nope", ""),
        ("", "Other"),
        ("", "OTHER"),
        ("", "$$$/Patterns/Defaults/Water=WATER"),
        ("", "café"),
        ("", "Café"),
        ("", "Café\u{0}\u{0}"),
        ("#8", "x"),
        ("", "gray"),
        ("", "Missing"),
    ];
    for (id, name) in lookups {
        let real = id.strip_prefix('#').map_or(id.to_string(), |i| pats[i.parse::<usize>().unwrap()].id.clone());
        let found = pattern::find(&pats, &real, name).map(|p| pats.iter().position(|q| std::ptr::eq(q, p)).unwrap());
        l!("pattern_find", id, name.replace('\0', "\\0"), opt(found));
    }
}

fn analysis_fns() {
    let vals = [
        1.0f64, 0.0, -0.0, 100.0, 2.5, 0.1, 1e20, 1e-7, 1e300, -3.0, -2.5, f64::NAN, f64::INFINITY, f64::NEG_INFINITY, 0.03125, 0.09375, 0.15625, 1.65625,
        -0.03125, 0.00005, 2.00015, 9.99995, 123456789.123456789, 9.223372036854776e18, -9.3e18, 4503599627370497.0, 0.5, 1e15 + 0.5,
    ];
    for (i, &p) in vals.iter().enumerate() {
        let lg = vals[(i * 7 + 3) % vals.len()];
        for (pl, ll) in [(p, 1.0), (1.0, p), (p, lg)] {
            let units = if i % 3 == 0 { "pixels" } else if i % 3 == 1 { "µm" } else { "" };
            let s = MeasurementScale { pixel_length: pl, logical_length: ll, units: units.into() };
            l!("scale", x64(pl), x64(ll), units, s.describe(), x64(s.factor()), s.is_default());
        }
    }
    for m in [Measurement::default(), measurement_full()] {
        l!("measurement", js!(&m), m.count_total(), m.is_empty());
    }
}

fn timeline_fns() {
    for (d, fps) in [(0usize, 0.0f32), (24, 24.0), (1, f32::NAN), (usize::MAX, f32::INFINITY), (5, -1.0), (3, 1e-7), (2, sub1())] {
        l!("timeline_new", d, x32(fps), js!(&Timeline::new(d, fps)));
    }
    let cases: [(f32, usize, usize, usize, usize); 7] = [
        (24.0, 24, 100, 0, 999),
        (f32::NAN, 0, 5, 7, 0),
        (f32::NEG_INFINITY, usize::MAX, usize::MAX, usize::MAX, usize::MAX),
        (30.0, 10, 3, 9, 2),
        (30.0, 10, 3, 4, 7),
        (0.0, 1, 0, 0, 1),
        (12.5, 3, 2, 2, 3),
    ];
    for (fps, d, c, ws, we) in cases {
        let mut t = Timeline { fps, duration: d, current: c, work_start: ws, work_end: we };
        let before = format!("{},{},{},{},{}", x32(fps), d, c, ws, we);
        let time0 = x32(t.time());
        t.clamp();
        l!("timeline_clamp", before, time0, js!(&t), x32(t.time()));
    }
    for (d, c, deltas) in [(3usize, 0usize, "-1,1,9223372036854775807,-9223372036854775808"), (0, usize::MAX, "1,0,-1"), (usize::MAX, 5, "9223372036854775807,9223372036854775807,9223372036854775807,-1"), (10, 4, "2,-3,-100,100,0")] {
        let mut t = Timeline { fps: 30.0, duration: d, current: c, work_start: 0, work_end: 1 };
        let mut out = Vec::new();
        for x in deltas.split(',') {
            t.step(x.parse().unwrap());
            out.push(t.current.to_string());
        }
        l!("timeline_step", d, c, deltas, out.join(","));
    }
    for (fps, c) in [(24.0f32, 23usize), (1e-7, 1), (0.0, 7), (30.0, usize::MAX), (29.97, 16777217), (f32::NAN, 1), (-5.0, 3)] {
        let t = Timeline { fps, duration: 100, current: c, work_start: 0, work_end: 1 };
        l!("timeline_time", x32(fps), c, x32(t.time()));
    }
}

fn text_fns() {
    let run = |len: usize, size: f32| TextRun { len, style: CharStyle { size_pt: size, ..Default::default() } };
    let prun = |len: usize, a: TextAlign| ParagraphRun { len, style: ParagraphStyle { align: a, ..Default::default() } };
    let cases: Vec<(&str, Vec<TextRun>, Vec<ParagraphRun>)> = vec![
        ("héllo", vec![], vec![]),
        ("héllo", vec![run(2, 10.0), run(1, 20.0)], vec![prun(1, TextAlign::Center)]),
        ("héllo", vec![run(0, 1.0), run(100, 10.0), run(5, 20.0)], vec![prun(0, TextAlign::Right), prun(0, TextAlign::Center)]),
        ("", vec![run(0, 33.0)], vec![]),
        ("", vec![], vec![prun(5, TextAlign::JustifyAll)]),
        ("ab", vec![run(1, 1.0), run(usize::MAX, 2.0), run(usize::MAX, 3.0)], vec![prun(usize::MAX, TextAlign::Right)]),
        ("日本語", vec![run(1, 5.0), run(1, 6.0), run(1, 7.0), run(1, 8.0)], vec![prun(4, TextAlign::Right), prun(2, TextAlign::Center)]),
        ("a😀b", vec![run(2, 5.0), run(2, 6.0)], vec![prun(5, TextAlign::Right)]),
        ("a\r\nb\u{2029}c", vec![run(0, 1.0)], vec![prun(3, TextAlign::Center), prun(0, TextAlign::Left), prun(3, TextAlign::Right)]),
        ("xyz", vec![run(0, 1.0), run(0, 2.0)], vec![prun(1, TextAlign::Center)]),
    ];
    for (text, runs, paras) in cases {
        let mut t = TextLayer { text: text.into(), font_family: "Fam".into(), size_pt: 9.0, color: Color::rgb(0.1, 0.2, 0.3), runs, paragraphs: paras, ..Default::default() };
        let cr = t.char_runs();
        let pr = t.paragraph_runs();
        l!("text_runs", text.replace('\r', "\\r").replace('\n', "\\n"), js!(&t.runs), js!(&t.paragraphs), js!(&cr), js!(&pr));
        t.sync_summary();
        l!("text_sync_summary", js!(&t.runs), &t.font_family, x32(t.size_pt), js!(&t.color));
    }
}

fn opt_s(v: Option<u32>) -> String { optid(v) }

fn styles_fns() {
    let s0 = TextStyles::default();
    let s1 = styles_full();
    let mut s2 = styles_full();
    s2.char_style_mut(1).unwrap().attrs.insert("size_pt".into(), json!(50.0));
    s2.paragraph[0].char_attrs.insert("italic".into(), json!(true));
    s2.basic.para_attrs.insert("hyphenate".into(), json!(true));
    // A style whose attrs don't apply (wrong type, unknown key): resolution keeps the base.
    let mut s3 = styles_full();
    s3.character.push(CharacterStyleDef { id: 2, name: "Bad".into(), attrs: attrs(json!({"size_pt": "big"})) });
    s3.character.push(CharacterStyleDef { id: 3, name: "Unknown".into(), attrs: attrs(json!({"nope": 1, "style_sheet": 9})) });
    s3.paragraph.push(ParagraphStyleDef { id: 2, name: "BadP".into(), para_attrs: attrs(json!({"align": "Diagonal"})), char_attrs: attrs(json!({"weight": -1})) });
    s3.basic.char_attrs.insert("size_pt".into(), json!(null));
    let all = [&s0, &s1, &s2, &s3];
    let ids: [Option<u32>; 6] = [None, Some(0), Some(1), Some(2), Some(3), Some(7)];
    for (si, s) in all.iter().enumerate() {
        l!("ts_styles", si, js!(s), s.is_empty(), opt_s(s.next_char_id()), opt_s(s.next_para_id()));
        for p in ids {
            l!("ts_resolve_para", si, optid(p), js!(&s.resolve_para(p)));
            for c in ids {
                l!("ts_resolve_char", si, optid(p), optid(c), "Inter", js!(&s.resolve_char(p, c, "Inter")));
            }
        }
        for (para, base) in [(false, "Character Style"), (true, "Paragraph Style"), (false, "None"), (true, "Basic"), (false, ""), (true, "Head")] {
            l!("ts_unique_name", si, para, base, s.unique_name(para, base));
        }
        for id in [0u32, 1, 2, 7, u32::MAX] {
            l!("ts_lookup", si, id, opt(s.char_style(id).map(|d| js!(&d.name))), opt(s.para_style(id).map(|d| js!(&d.name))));
        }
    }
    let mut named = styles_full();
    named.character.push(CharacterStyleDef { id: 5, name: "Character Style 1".into(), attrs: StyleAttrs::new() });
    named.character.push(CharacterStyleDef { id: 6, name: "Character Style 3".into(), attrs: StyleAttrs::new() });
    named.paragraph.push(ParagraphStyleDef { id: 9, name: "Paragraph Style 1".into(), ..Default::default() });
    l!("ts_unique_name_named", js!(&named), named.unique_name(false, "Character Style"), named.unique_name(true, "Paragraph Style"));

    let runs = vec![
        CharStyle::default(),
        char_full(),
        CharStyle { font_family: "Inter".into(), size_pt: 40.0, underline: true, style_sheet: Some(1), italic: true, ..Default::default() },
        CharStyle { font_family: "Inter".into(), size_pt: 20.0, weight: 700, color: Color::rgb(1.0, 0.0, 0.0), style_sheet: Some(9), ..Default::default() },
    ];
    for (ri, r) in runs.iter().enumerate() {
        l!("ts_run", ri, js!(r));
    }
    for (si, s) in all.iter().enumerate() {
        for (ri, r) in runs.iter().enumerate() {
            for p in [None, Some(7), Some(2)] {
                l!("ts_char_overrides", si, ri, optid(p), "Inter", js!(&s.char_overrides(r, p, "Inter")));
            }
        }
    }
    let paras = vec![ParagraphStyle::default(), para_full(), ParagraphStyle { align: TextAlign::Center, style_sheet: Some(7), ..Default::default() }, ParagraphStyle { style_sheet: Some(2), hyphenate: true, ..Default::default() }];
    for (pi, p) in paras.iter().enumerate() {
        l!("ts_para", pi, js!(p));
    }
    for (si, s) in all.iter().enumerate() {
        for (pi, p) in paras.iter().enumerate() {
            l!("ts_para_overrides", si, pi, js!(&s.para_overrides(p)));
        }
    }
    // restyle: (new, old) pairs.
    for (ni, oi) in [(2usize, 1usize), (1, 2), (0, 1), (3, 1), (1, 3)] {
        let (new, old) = (all[ni], all[oi]);
        for (ri, r) in runs.iter().enumerate() {
            for (op, np, nc) in [(None, None, Some(1)), (None, None, None), (Some(7), Some(7), Some(1)), (Some(7), None, Some(2)), (None, Some(2), Some(3))] {
                l!("ts_restyle_char", ni, oi, ri, optid(op), optid(np), optid(nc), "Inter", js!(&new.restyle_char(old, r, op, np, nc, "Inter")));
            }
        }
        for (pi, p) in paras.iter().enumerate() {
            for np in [None, Some(0), Some(7), Some(2)] {
                l!("ts_restyle_para", ni, oi, pi, optid(np), js!(&new.restyle_para(old, p, np)));
            }
        }
    }
    // Attribute sets.
    let sets = [
        json!({}),
        json!({"size_pt": 3.0}),
        json!({"size_pt": "big"}),
        json!({"nope": 1}),
        json!({"style_sheet": "ignored"}),
        json!({"weight": 70000}),
        json!({"weight": 700, "italic": true, "kerning": "Off"}),
        json!({"kerning": "off"}),
        json!({"align": "Center"}),
        json!({"auto_leading": null}),
        json!({"leading_pt": null, "language": "fr"}),
        json!({"color": {"mode": "Rgb", "c": [1, 0, 0, 0], "alpha": 1}}),
        json!({"color": {"mode": "Rgb"}}),
        json!({"we\"ird é": 1}),
        json!({"size_pt": 1e300}),
        json!({"features": [{"tag": "liga", "value": 0}], "variations": []}),
        json!({"direction": "Rtl", "hyphenate": true, "z": 0}),
        json!({"first_line_indent_pt": 1, "space_after_pt": -2.5}),
    ];
    for a in &sets {
        let m = attrs(a.clone());
        let r = |x: Result<(), String>| x.map_or_else(|e| format!("ERR {}", trunc(e)), |_| "OK".to_string());
        l!("ts_validate", js!(&m), r(text_styles::validate::<CharStyle>(&m)), r(text_styles::validate::<ParagraphStyle>(&m)));
        let ac = text_styles::apply_attrs(&char_full(), &m).map_or_else(|e| format!("ERR {}", trunc(e)), |v| js!(&v));
        let ap = text_styles::apply_attrs(&para_full(), &m).map_or_else(|e| format!("ERR {}", trunc(e)), |v| js!(&v));
        l!("ts_apply", js!(&m), ac, ap);
    }
    l!("ts_all_attrs", js!(&text_styles::char_attrs(&char_full())), js!(&text_styles::para_attrs(&para_full())));
    l!("ts_diff_attrs", js!(&text_styles::diff_attrs(&char_full(), &CharStyle::default())), js!(&text_styles::diff_attrs(&para_full(), &ParagraphStyle::default())),
       js!(&text_styles::diff_attrs(&CharStyle { style_sheet: Some(3), ..Default::default() }, &CharStyle::default())));
}

// Tree spec: a string is a raster layer; {"n":name,"c":[children]?,"l":[t,p,pos,ab,all]?,"k":link?}.
fn build_layer(v: &J) -> Layer {
    if let Some(s) = v.as_str() {
        return Layer::raster(s, PixelFormat::RGBA8);
    }
    let name = v["n"].as_str().unwrap();
    let mut l = match v.get("c") {
        Some(c) => Layer::group(name, c.as_array().unwrap().iter().map(build_layer).collect()),
        None => Layer::raster(name, PixelFormat::RGBA8),
    };
    if let Some(lk) = v.get("l") {
        let b: Vec<bool> = lk.as_array().unwrap().iter().map(|x| x.as_bool().unwrap()).collect();
        l.locks = Locks { transparency: b[0], pixels: b[1], position: b[2], artboard: b[3], all: b[4] };
    }
    if let Some(k) = v.get("k") {
        l.link_group = Some(k.as_u64().unwrap());
    }
    l
}

fn build_doc(name: &str, w: u32, h: u32, tree: &str) -> Document {
    let mut d = Document::new(name, Size::new(w, h), ColorMode::Rgb, SampleType::U8);
    let t: J = serde_json::from_str(tree).unwrap();
    d.layers = t.as_array().unwrap().iter().map(build_layer).collect();
    d
}

fn doc_fns() {
    let trees = [
        r#"[]"#,
        r#"["Background","Layer 1","Layer 2"]"#,
        r#"["Layer 1",{"n":"Group 1","c":["Layer 2",{"n":"Layer 3","k":7},"Layer 1 copy"]},"Layer 1 copy 2"]"#,
        r#"[{"n":"G","c":[{"n":"G","c":[{"n":"G","c":["L"]}]}]},{"n":"empty","c":[]}]"#,
        r#"[{"n":"a","k":3},{"n":"b","k":18446744073709551614}]"#,
        r#"[{"n":"a","k":18446744073709551615}]"#,
        r#"[{"n":"a","k":0},{"n":"b","k":1},{"n":"c","k":2},{"n":"d","k":4},{"n":"e","k":18446744073709551615}]"#,
        r#"[{"n":"G","l":[true,false,false,false,false],"c":[{"n":"H","l":[false,true,false,true,false],"c":[{"n":"x","l":[false,false,true,false,false]},"y"]},{"n":"z","l":[false,false,false,false,true]}]},"Layer 1 copy","Photo copy 3"]"#,
        r#"[{"n":"G","c":[{"n":"","c":[]}]},"日本 copy","copy","copy copy"," copy"]"#,
    ];
    for t in trees {
        let d = build_doc("t", 10, 10, t);
        l!("doc_depth", t, d.max_group_depth(), d.layer_count());
        l!("doc_link", t, opt(d.unused_link_group()));
        for base in ["Layer", "Group", "", "日本"] {
            l!("doc_next_name", t, base, d.next_layer_name(base));
        }
        for n in ["Layer 1", "Layer 1 copy", "Layer 1 copy 2", "Layer 1 copy x", "Layer 1 copy 03", "Layer 1 copy ", "copy", " copy", "Photo copy 3", "日本", "日本 copy", "a copy b copy", "", "Layer 1 copy 2 copy"] {
            l!("doc_copy_name", t, n, d.copy_name(n));
        }
        let mut paths: Vec<Vec<usize>> = d.walk().into_iter().map(|(p, _, _)| p).collect();
        paths.extend([vec![], vec![99], vec![0, 99], vec![0, 0, 0, 0, 0]]);
        for p in paths {
            let ps = p.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(".");
            l!("doc_locks", t, ps, js!(&d.locks_at(&p)));
        }
    }
    // Slices.
    let canvases = [Rect::new(0, 0, 100, 80), Rect::new(0, 0, 0, 0), Rect::new(-10, -10, 10, 10)];
    let takens = [
        r#"[]"#,
        r#"[{"x0":20,"y0":10,"x1":60,"y1":30}]"#,
        r#"[{"x0":20,"y0":10,"x1":60,"y1":30},{"x0":50,"y0":20,"x1":120,"y1":90}]"#,
        r#"[{"x0":0,"y0":0,"x1":100,"y1":80}]"#,
        r#"[{"x0":-50,"y0":-50,"x1":5,"y1":5},{"x0":5,"y0":5,"x1":5,"y1":50},{"x0":200,"y0":200,"x1":300,"y1":300}]"#,
        r#"[{"x0":10,"y0":0,"x1":20,"y1":80},{"x0":30,"y0":0,"x1":40,"y1":80},{"x0":0,"y0":40,"x1":100,"y1":50}]"#,
        r#"[{"x0":0,"y0":0,"x1":10,"y1":10},{"x0":0,"y0":0,"x1":10,"y1":10},{"x0":90,"y0":70,"x1":100,"y1":80}]"#,
    ];
    for c in canvases {
        for t in takens {
            let taken: Vec<Rect> = serde_json::from_str(t).unwrap();
            l!("auto_slices", js!(&c), t, js!(&slices::auto_slices(c, &taken)));
        }
    }
    let docs = [
        ("site.psd", 100u32, 80u32, r#"{}"#),
        ("site.psd", 100, 80, r#"{"list":[{"id":1,"rect":{"x0":20,"y0":10,"x1":60,"y1":30}}]}"#),
        ("site.psd", 100, 80, r#"{"list":[{"id":1,"name":"logo","rect":{"x0":20,"y0":10,"x1":60,"y1":30},"kind":"table","origin":"layer"},{"id":2,"rect":{"x0":0,"y0":0,"x1":100,"y1":10}},{"id":3,"rect":{"x0":500,"y0":0,"x1":600,"y1":10}}]}"#),
        ("café 2024.psd", 10, 10, r#"{"list":[{"id":5,"rect":{"x0":0,"y0":0,"x1":10,"y1":10}}]}"#),
        ("a.b.c", 30, 30, r#"{"list":[{"id":5,"rect":{"x0":10,"y0":10,"x1":20,"y1":20}},{"id":5,"name":"dup","rect":{"x0":0,"y0":0,"x1":5,"y1":5}}]}"#),
        ("", 0, 0, r#"{"list":[{"id":1,"rect":{"x0":0,"y0":0,"x1":5,"y1":5}}]}"#),
        ("日本.png", 4, 4, r#"{}"#),
        (".psd", 4, 4, r#"{"list":[{"id":9,"rect":{"x0":-5,"y0":-5,"x1":2,"y1":2},"kind":"noImage"}]}"#),
    ];
    for (name, w, h, sl) in docs {
        let mut d = build_doc(name, w, h, "[]");
        d.slices = serde_json::from_str(sl).unwrap();
        let r: Vec<String> = slices::resolve(&d)
            .iter()
            .map(|s| format!("{}|{}|{}|{}|{}|{}", s.number, optid(s.id), s.origin.id(), js!(&s.rect), s.name, s.kind.id()))
            .collect();
        l!("slices_resolve", name, format!("{w}x{h}"), sl, r.join(";"), opt_s(d.slices.next_id()));
    }
    for n in ["café 2024.psd", "日本.png", "a.b.c", ".psd", "", "site", "x-y_z.v2.tif", "a b\u{301}c.png", "...", "a.", "Ünïcödé ½ ① Ⅻ.psd", "😀.png", "tab\u{3000}x.jpg"] {
        let d = build_doc(n, 1, 1, "[]");
        l!("slices_base_name", n, slices::base_name(&d));
    }
    for sl in [r#"{}"#, r#"{"list":[{"id":4294967295}]}"#, r#"{"list":[{"id":3},{"id":9},{"id":1}]}"#] {
        let s: Slices = serde_json::from_str(sl).unwrap();
        l!("slices_next_id", sl, opt_s(s.next_id()), s.get(9).map_or("None".into(), |x| js!(x)), s.is_empty());
    }
    // Variables lookups.
    let v = variables_full();
    l!("vars_full", js!(&v));
    for n in ["show", "photo", "missing", "", "row1", "empty"] {
        l!("vars_lookup", n, opt(v.def(n).map(|x| js!(x))), opt(v.data_set(n).map(|x| js!(x))), opt(v.data_set_index(n)), opt(v.data_sets[0].values.iter().find(|x| x.variable == n).map(|x| js!(x))));
    }
    l!("vars_is_empty", Variables::default().is_empty(), v.is_empty(), Variables { active: Some(1), ..Default::default() }.is_empty());
}

fn nonfinite_fns() {
    l!("nonfinite", "CurvePoint", js!(&CurvePoint { input: f32::NAN, output: f32::INFINITY }));
    l!("nonfinite", "Point", js!(&Point::new(f64::NAN, f64::NEG_INFINITY)));
    l!("nonfinite", "Color", js!(&Color::rgba(f32::NAN, f32::INFINITY, f32::NEG_INFINITY, 0.5)));
    l!("nonfinite", "MeasurementScale", js!(&MeasurementScale { pixel_length: f64::NAN, logical_length: f64::INFINITY, units: "x".into() }));
    l!("nonfinite", "Timeline", js!(&Timeline { fps: f32::NAN, duration: 1, current: 0, work_start: 0, work_end: 1 }));
    l!("nonfinite", "GlobalLight", js!(&GlobalLight { angle: f32::INFINITY, altitude: f32::NAN }));
    l!("nonfinite", "Guides", js!(&Guides { horizontal: vec![f64::NAN, 1.0], vertical: vec![f64::NEG_INFINITY], colors: Vec::new() }));
    l!("nonfinite", "CharStyleValue", js!(&serde_json::to_value(CharStyle { size_pt: f32::NAN, leading_pt: Some(f32::INFINITY), ..Default::default() }).unwrap()));
}

// ---------------------------------------------------------------------------------------------
// R4 (Phase 5R resync): the fields and functions upstream added after efa62efa. Appended last so
// every earlier line keeps its place.

fn h32s(v: impl IntoIterator<Item = f32>) -> String {
    let mut h = H0;
    for x in v {
        fnv(&mut h, &x.to_bits().to_le_bytes());
    }
    format!("{h:016x}")
}

fn resync_lines() {
    use photocraft_doc::adjust::{curve_map, curve_map_outputs};
    use photocraft_doc::{FileEncoding, GuideColor};
    ser!(Adjustment: Adjustment::Curves {
        master: curve(&[(0.0, 0.2), (1.0, 0.8)]),
        per_channel: [vec![], curve(&[(0.5, 0.25)]), vec![]],
        space: ToneSpace::Cmyk,
        black: vec![],
        pencil: true
    });
    ser!(Contour:
        Contour::Custom { name: "c".into(), points: curve(&[(0.0, 0.0), (0.5, 1.0), (1.0, 0.0)]), corners: vec![false, true] },
        Contour::Custom { name: String::new(), points: vec![], corners: vec![true] });
    ser!(Guides: Guides {
        horizontal: vec![118.11023622047244, -0.5, 1e300],
        vertical: vec![7.03125],
        colors: vec![GuideColor { vertical: true, position: 7.03125, rgb: [0x33, 0x66, 0xff] }, GuideColor { vertical: false, position: f64::MAX, rgb: [0, 0, 255] }],
    });
    ser!(GuideColor: GuideColor { vertical: false, position: -0.0, rgb: [1, 2, 3] }, GuideColor { vertical: true, position: 5e-324, rgb: [255, 255, 255] });
    ser!(FileEncoding:
        FileEncoding::default(),
        FileEncoding { jpeg_quality: Some(87), ..Default::default() },
        FileEncoding { jpeg_quality: None, webp_lossless: Some(false), webp_quality: Some(100) },
        FileEncoding { jpeg_quality: Some(0), webp_lossless: Some(true), webp_quality: Some(255) });
    serp!(FileEncoding: FileEncoding { jpeg_quality: Some(12), webp_lossless: Some(true), webp_quality: None });
    serp!(Guides: Guides { horizontal: vec![1.5], vertical: vec![], colors: vec![GuideColor { vertical: false, position: 1.5, rgb: [9, 8, 7] }] });
    de!(Adjustment: r#"{"Curves":{"master":[],"per_channel":[[],[],[]],"pencil":true}}"#,
        r#"{"Curves":{"master":[],"per_channel":[[],[],[]],"pencil":1}}"#,
        r#"{"Curves":{"master":[],"per_channel":[[],[],[]],"pencil":null}}"#);
    de!(Contour: r#"{"Custom":{"name":"a","points":[]}}"#, r#"{"Custom":{"name":"a","points":[],"corners":[true,false,true]}}"#,
        r#"{"Custom":{"name":"a","points":[],"corners":null}}"#, r#"{"Custom":{"name":"a","points":[],"corners":[1]}}"#,
        r#"{"Custom":["a",[],[true]]}"#, r#"{"Custom":["a",[]]}"#);
    de!(Guides: r#"{"horizontal":[1],"vertical":[]}"#,
        r#"{"horizontal":[1.5],"vertical":[2],"colors":[{"vertical":true,"position":2,"rgb":[1,2,3]}]}"#,
        r#"{"horizontal":[],"vertical":[],"colors":null}"#,
        r#"{"horizontal":[],"vertical":[],"colors":[{"vertical":true,"position":2,"rgb":[1,2]}]}"#,
        r#"{"horizontal":[],"vertical":[],"colors":[{"vertical":true,"position":2,"rgb":[256,0,0]}]}"#,
        r#"{"horizontal":[1e300,-1.7976931348623157e308,18446744073709551615],"vertical":[0.30000000000000004]}"#,
        r#"[[1],[2],[{"vertical":false,"position":1,"rgb":[0,0,0]}]]"#, r#"[[1],[2]]"#);
    de!(GuideColor: r#"{"vertical":1,"position":0,"rgb":[0,0,0]}"#, r#"{"position":0,"rgb":[0,0,0]}"#,
        r#"{"vertical":false,"position":"x","rgb":[0,0,0]}"#, r#"[true,2.5,[1,2,3]]"#, r#"{"vertical":false,"position":1,"rgb":[0,0,0,0]}"#);
    de!(FileEncoding: r#"{}"#, r#"{"jpegQuality":90}"#, r#"{"jpegQuality":null,"webpLossless":true}"#, r#"{"jpegQuality":256}"#,
        r#"{"jpeg_quality":5}"#, r#"null"#, r#"[]"#, r#"[90]"#, r#"[90,true,50]"#, r#"[90,true,50,1]"#, r#"{"webpQuality":-1}"#,
        r#"{"webpLossless":"yes"}"#, r#"{"webpQuality":1.0}"#);

    // Pencil curve maps.
    let maps: Vec<Vec<f32>> = vec![
        vec![],
        vec![0.5],
        (0..256).map(|k| k as f32 / 255.0).collect(),
        (0..300).map(|k| ((k * 37) % 101) as f32 / 50.0 - 0.5).collect(),
        vec![f32::NAN, 0.25, f32::INFINITY, -0.0, 2.0, -1.0, sub1(), f32::NEG_INFINITY, 0.75],
        (0..255).map(|k| 1.0 - k as f32 / 254.0).collect(),
    ];
    for m in &maps {
        let pts = curve_map(m);
        l!("curve_map", xs32(m), pts.len(), h32s(pts.iter().flat_map(|p| [p.input, p.output])));
    }
    let pts: Vec<Vec<CurvePoint>> = vec![
        vec![],
        curve(&[(0.5, 0.5)]),
        curve(&[(0.0, 0.0), (1.0, 1.0)]),
        curve(&[(1.0, 0.0), (0.0, 1.0)]),
        curve(&[(0.2, 0.1), (0.2, 0.9), (0.8, 0.3)]),
        curve(&[(-0.0, 0.5), (0.0, 0.25), (1.0, 0.75)]),
        curve(&[(0.3, 0.6), (f32::NAN, 0.1), (0.7, f32::INFINITY), (0.9, 0.2)]),
        curve(&[(-1.0, -2.0), (0.5, 3.0), (2.0, 0.5)]),
        curve(&[(0.25, 0.0), (0.25, 1.0)]),
        curve(&[(0.1, 0.0), (0.4, 1.0), (0.4, 0.0), (0.6, 1.0), (0.6, 0.5), (0.95, 0.05)]),
        curve(&[(0.5, 1e30), (0.6, -1e30)]),
        curve(&[(0.0, 3e38), (1.0, -3e38)]),
    ];
    for c in &pts {
        let o = curve_map_outputs(c);
        let flat: Vec<f32> = c.iter().flat_map(|p| [p.input, p.output]).collect();
        l!("curve_map_outputs", xs32(&flat), o.len(), h32s(o.iter().copied()), xs32(&o[..o.len().min(8)]));
    }

    // Shape stroke limits.
    let base = ShapeStroke::default();
    let strokes: Vec<ShapeStroke> = vec![
        base.clone(),
        ShapeStroke { width: f32::NAN, ..base.clone() },
        ShapeStroke { width: -0.0, ..base.clone() },
        ShapeStroke { width: 1_000_000.0, ..base.clone() },
        ShapeStroke { width: 1_000_000.1, ..base.clone() },
        ShapeStroke { opacity: 1.0001, ..base.clone() },
        ShapeStroke { opacity: f32::INFINITY, ..base.clone() },
        ShapeStroke { miter_limit: 0.999, ..base.clone() },
        ShapeStroke { miter_limit: 500.0, ..base.clone() },
        ShapeStroke { dash_offset: -1_000_000.0, ..base.clone() },
        ShapeStroke { dash_offset: 1_000_000.5, ..base.clone() },
        ShapeStroke { dashes: vec![1.0; 32], ..base.clone() },
        ShapeStroke { dashes: vec![1.0; 33], ..base.clone() },
        ShapeStroke { dashes: vec![0.0, 0.005, 0.004], ..base.clone() },
        ShapeStroke { dashes: vec![0.0, 0.01], ..base.clone() },
        ShapeStroke { dashes: vec![10000.0, f32::NAN], ..base.clone() },
        ShapeStroke { dashes: vec![-0.0, 10000.01], ..base.clone() },
        ShapeStroke { dashes: vec![0.003, 0.003, 0.004], ..base.clone() },
    ];
    for st in &strokes {
        l!("stroke_validate", x32(st.width), x32(st.opacity), x32(st.miter_limit), x32(st.dash_offset), xs32(&st.dashes), st.validate().err().unwrap_or("ok"));
    }

    // Pattern size limits.
    for (w, h) in [(0u32, 5u32), (5, 0), (1, 1), (8192, 8192), (8193, 8192), (30000, 30000), (u32::MAX, u32::MAX), (1 << 26, 1), ((1 << 26) + 1, 1)] {
        let p = Pattern { id: String::new(), name: String::new(), width: w, height: h, surface: Surface::new(PixelFormat::RGBA8) };
        l!("pattern_paintable", w, h, p.pixel_count(), p.is_paintable());
    }
    l!("pattern_limits", pattern::MAX_EDGE, pattern::MAX_PAINT_PIXELS);

    // Paragraph runs keep the empty paragraph after a trailing line break.
    let run = |len: usize, size: f32| TextRun { len, style: CharStyle { size_pt: size, ..Default::default() } };
    let prun = |len: usize, a: TextAlign| ParagraphRun { len, style: ParagraphStyle { align: a, ..Default::default() } };
    let cases: Vec<(&str, Vec<TextRun>, Vec<ParagraphRun>)> = vec![
        ("ab\r", vec![run(3, 1.0)], vec![prun(3, TextAlign::Center), prun(0, TextAlign::Right)]),
        ("ab\r", vec![], vec![prun(3, TextAlign::Center), prun(1, TextAlign::Right)]),
        ("ab\r", vec![], vec![prun(3, TextAlign::Center), prun(2, TextAlign::Right)]),
        ("ab\n", vec![], vec![prun(2, TextAlign::Center), prun(1, TextAlign::Right)]),
        ("ab\n", vec![], vec![prun(1, TextAlign::Center), prun(1, TextAlign::Left), prun(1, TextAlign::Right)]),
        ("a\rb\r", vec![], vec![prun(2, TextAlign::Center), prun(2, TextAlign::Left), prun(0, TextAlign::JustifyAll)]),
        ("ab", vec![], vec![prun(2, TextAlign::Center), prun(0, TextAlign::Right)]),
        ("\n", vec![], vec![prun(1, TextAlign::Center), prun(0, TextAlign::Right)]),
        ("\n", vec![], vec![prun(0, TextAlign::Right)]),
        ("ab\r", vec![], vec![prun(usize::MAX, TextAlign::Center), prun(0, TextAlign::Right)]),
        ("é\r", vec![], vec![prun(3, TextAlign::Center), prun(1, TextAlign::Right)]),
    ];
    for (text, runs, paras) in cases {
        let text = text.to_string();
        let t = TextLayer { text: text.clone(), font_family: "Fam".into(), size_pt: 9.0, color: Color::rgb(0.1, 0.2, 0.3), runs, paragraphs: paras, ..Default::default() };
        let cr = t.char_runs();
        let pr = t.paragraph_runs();
        l!("text_runs", text.replace('\r', "\\r").replace('\n', "\\n"), js!(&t.runs), js!(&t.paragraphs), js!(&cr), js!(&pr));
    }

    // Colours into a document mode, and switching an effect on and off.
    let modes = [ColorMode::Rgb, ColorMode::Grayscale, ColorMode::Cmyk, ColorMode::Lab, ColorMode::Indexed, ColorMode::Bitmap, ColorMode::Duotone, ColorMode::Multichannel];
    for e in effects_all() {
        for m in modes {
            l!("effect_in_mode", js!(&e), js!(&m), js!(&e.clone().in_mode(m)));
        }
        let mut off = e.clone();
        off.set_enabled(!e.enabled());
        l!("effect_set_enabled", js!(&e), !e.enabled(), js!(&off));
    }
    for f in fills() {
        for m in modes {
            l!("fill_in_mode", js!(&f), js!(&m), js!(&f.clone().in_mode(m)));
        }
    }
    let cc = Contour::Custom { name: "k".into(), points: curve(&[(0.0, 0.0), (0.5, 1.0), (1.0, 0.0)]), corners: vec![false, true] };
    for (c, i) in [(&cc, 0usize), (&cc, 1), (&cc, 2), (&cc, usize::MAX), (&Contour::Linear, 0)] {
        l!("contour_is_corner", js!(c), i, c.is_corner(i));
    }
}

pub fn doc_vectors() {
    ser_lines();
    de_lines();
    hue_fns();
    blend_fns();
    adjust_fns();
    mode_fns();
    label_fns();
    mask_fns();
    path_fns();
    pattern_fns();
    analysis_fns();
    timeline_fns();
    text_fns();
    styles_fns();
    doc_fns();
    nonfinite_fns();
    resync_lines();
}
