//! I3 PSD export / smart objects reference vectors (`rustref io-export-vectors`), checked by
//! tests/io/test_export_psd_parity.c. Lines are tab-separated; the first field names the kind.
//!
//!   doc NAME B64                     a document, as a .pcraft bundle (loaded with its ids kept;
//!                                    the C side exports the same bundle)
//!   psd NAME VARIANT RESULT OUTLINE  document_to_psd_with of that document; VARIANT d (default),
//!                                    b (force_psb), t (no matte, raw merged: layered TIFF)
//!   rt NAME RESULT OUTLINE           export -> PsdFile::from_bytes -> psd_to_document -> export
//!   rt2 NAME RESULT                  once more (fixed point)
//!   sfw CASE RESULT                  smart_map::item_for_filter: `ok HEX` / `err "reason"`
//!   sfr CASE FILTER                  smart_map::filter_from_item of that item (or of a raw case)
//!   sff CASE HEX W[...]              smart_map::filter_fx of a stack
//!   sold CASE HEX W | plld CASE HEX W   sold_bytes / plld_bytes of a spec
//!   psold CASE RESULT                parse_sold
//!   mask CASE RESULT                 mask_from_item
//!   feid CASE RESULT                 feid_item (FEid bytes of the item, F32 normalised)
//!   uuid HEX UUID                    uuid_from
//!   corp PATH RESULT                 corpus file: io::import then document_to_psd_with
//!
//! RESULT of an export: `ok LEN HASH ZIP NHASH W[warnings]`: the file's length and BLAKE3 (16 hex),
//! whether any channel, merged image or FEid plane is zip-compressed (zlib output differs between
//! the implementations), and the BLAKE3 of the file with those planes stored raw (normalised).
//! OUTLINE lists the normalised file's parts (for diagnosing a mismatch).
use std::sync::Arc;

use base64::Engine;
use photocraft_color::{BlendMode, Color, ColorMode, PixelFormat, SampleType};
use photocraft_doc::*;
use photocraft_geom::{Affine, Rect, Size};
use photocraft_io::smart_map::{self, FilterStack, PlacedSpec};
use photocraft_io::{PsdExportOptions, document_to_psd_with, psd_to_document};
use photocraft_psd::filter_effects::{EffectsPlane, FilterEffects, FilterEffectsItem};
use photocraft_psd::{Compression, ImageData, PsdFile};
use photocraft_raster::Surface;
use serde_json::json;

#[path = "../../../crates/io/tests/common/mod.rs"]
mod common;

use common::{Features, gen_doc, mask_surface, pattern, raster};

fn h16(b: &[u8]) -> String {
    blake3::hash(b).to_hex()[..16].to_string()
}
fn h8(b: &[u8]) -> String {
    blake3::hash(b).to_hex()[..8].to_string()
}
fn b64(b: &[u8]) -> String {
    base64::engine::general_purpose::STANDARD.encode(b)
}
fn hexs(b: &[u8]) -> String {
    b.iter().map(|v| format!("{v:02x}")).collect()
}
fn warn_s(w: &[String]) -> String {
    format!("W{w:?}")
}

// ---------------------------------------------------------------------------------------------
// normalising and describing a written file
// ---------------------------------------------------------------------------------------------

fn is_zip(c: Compression) -> bool {
    matches!(c, Compression::Zip | Compression::ZipPrediction)
}

fn raw_plane(p: &EffectsPlane, w: usize, h: usize, depth: u16, zip: &mut bool) -> EffectsPlane {
    if !is_zip(p.compression) {
        return p.clone();
    }
    *zip = true;
    match p.decode(w, h, depth) {
        Ok(d) => EffectsPlane { compression: Compression::Raw, data: d },
        Err(_) => p.clone(),
    }
}

/// The file with every zip-compressed plane stored raw; and whether there was one.
fn normalize(f: &PsdFile) -> (PsdFile, bool) {
    let mut f = f.clone();
    let mut zip = false;
    let v = f.header.version;
    let depth = f.header.depth;
    if let Some(li) = &mut f.layer_info {
        for l in &mut li.layers {
            let rects: Vec<_> = l.channels.iter().map(|c| l.channel_rect(c.id)).collect();
            for (c, r) in l.channels.iter_mut().zip(rects) {
                if c.compression.is_some_and(is_zip) {
                    zip = true;
                    if let Some(d) = r.size().ok().and_then(|(w, h)| c.decode(w, h, depth, v).ok()) {
                        c.compression = Some(Compression::Raw);
                        c.data = d;
                    }
                }
            }
        }
    }
    // The layer info's 4-byte padding depends on the compressed lengths: recompute it.
    if zip {
        if let Some(li) = &mut f.layer_info {
            if li.pad_to(v, 4).is_err() {
                li.padding = None;
            }
        }
    }
    if is_zip(f.image_data.compression) {
        zip = true;
        if let Ok(d) = f.image_data.decode(&f.header) {
            f.image_data = ImageData { compression: Compression::Raw, data: d };
        }
    }
    for b in &mut f.global_blocks {
        if &b.key != b"FEid" && &b.key != b"FXid" {
            continue;
        }
        let Ok(mut fx) = FilterEffects::parse(&b.data) else { continue };
        let mut any = false;
        for it in &mut fx.items {
            let (Ok((w, h)), Ok(d)) = (it.rect.size(), u16::try_from(it.depth)) else { continue };
            for s in it.slots.iter_mut().flatten() {
                *s = raw_plane(s, w, h, d, &mut any);
            }
            if let Some((r, p)) = &mut it.mask {
                if let Ok((mw, mh)) = r.size() {
                    *p = raw_plane(p, mw, mh, d, &mut any);
                }
            }
        }
        if any {
            zip = true;
            b.data = fx.to_bytes();
        }
    }
    (f, zip)
}

fn outline(f: &PsdFile) -> String {
    let h = &f.header;
    let mut s = format!(
        "H{}:{}:{}x{}:{}:{} cmd={}:{}",
        h.version.as_u16(),
        h.channels,
        h.width,
        h.height,
        h.depth,
        h.color_mode.as_u16(),
        f.color_mode_data.len(),
        h8(&f.color_mode_data)
    );
    s.push_str(" R[");
    for r in &f.resources {
        s.push_str(&format!("{}:{}:{}:{} ", r.id, hexs(&r.name), r.data.len(), h8(&r.data)));
    }
    s.push(']');
    if let Some(li) = &f.layer_info {
        s.push_str(&format!(" LI{}:{:?}", u8::from(li.merged_alpha), li.padding.as_ref().map(Vec::len)));
        for l in &li.layers {
            let m = match &l.mask {
                photocraft_psd::layer::MaskData::None => "-".to_string(),
                photocraft_psd::layer::MaskData::Raw(r) => format!("raw{}", r.len()),
                photocraft_psd::layer::MaskData::Mask(m) => format!(
                    "{},{},{},{}:{}:{}:{}:{}",
                    m.rect.top,
                    m.rect.left,
                    m.rect.bottom,
                    m.rect.right,
                    m.default_color,
                    m.flags,
                    m.parameters.as_ref().map_or(0, |p| p.flags),
                    m.trailing.len()
                ),
            };
            s.push_str(&format!(
                " L{{{}|{},{},{},{}|{}|{}|{}|{}|{}|{}|{}|C",
                hexs(&l.name),
                l.rect.top,
                l.rect.left,
                l.rect.bottom,
                l.rect.right,
                String::from_utf8_lossy(&l.blend_mode.key()),
                l.opacity,
                l.clipping,
                l.flags.0,
                l.filler,
                m,
                h8(&l.blending_ranges.data)
            ));
            for c in &l.channels {
                s.push_str(&format!(" {}:{}:{}:{}", c.id, c.compression.map_or(-1, |c| i32::from(c.as_u16())), c.data.len(), h8(&c.data)));
            }
            s.push_str("|B");
            for b in &l.blocks {
                s.push_str(&format!(" {}:{}:{}:{:?}", String::from_utf8_lossy(&b.key), b.data.len(), h8(&b.data), b.padding.as_ref().map(Vec::len)));
            }
            s.push('}');
        }
    }
    s.push_str(&format!(" P{:?} GM{:?} G[", f.layer_info_placement, f.global_layer_mask.as_ref().map(|g| g.data.len())));
    for b in &f.global_blocks {
        s.push_str(&format!(
            "{}{}:{}:{}:{:?} ",
            String::from_utf8_lossy(&b.signature),
            String::from_utf8_lossy(&b.key),
            b.data.len(),
            h8(&b.data),
            b.padding.as_ref().map(Vec::len)
        ));
    }
    s.push_str(&format!("] I{}:{}:{}", f.image_data.compression.as_u16(), f.image_data.data.len(), h8(&f.image_data.data)));
    s
}

/// `ok LEN HASH ZIP NHASH W[..]` and the outline.
fn psd_result(f: &PsdFile, w: &[String]) -> (String, String) {
    match f.to_bytes() {
        Err(e) => (format!("err psd {:?}", e.to_string()), "-".into()),
        Ok(bytes) => {
            let (n, zip) = normalize(f);
            let nb = n.to_bytes().unwrap_or_default();
            (format!("ok {} {} {} {} {}", bytes.len(), h16(&bytes), u8::from(zip), h16(&nb), warn_s(w)), outline(&n))
        }
    }
}

// ---------------------------------------------------------------------------------------------
// documents
// ---------------------------------------------------------------------------------------------

fn g(v: u8) -> f32 {
    f32::from(v) / 255.0
}

fn sf(command: &str, params: serde_json::Value) -> SmartFilter {
    SmartFilter { command: command.into(), params, blend: BlendMode::Normal, opacity: 1.0, visible: true }
}

fn rect_path(x0: f64, y0: f64, x1: f64, y1: f64) -> Path {
    Path::new(vec![Subpath::polygon(&[(x0, y0), (x1, y0), (x1, y1), (x0, y1)])])
}

fn png_bytes(w: u32, h: u32, seed: u32) -> Vec<u8> {
    let mut d = Document::new("src", Size::new(w, h), ColorMode::Rgb, SampleType::U8);
    d.layers.push(raster("px", PixelFormat::RGBA8, Rect::new(0, 0, w as i32, h as i32), seed, true));
    photocraft_io::export(&d, "png", &photocraft_io::ExportOptions::default()).expect("png").bytes
}

fn smart(source: SmartSource, transform: Affine, cache: Option<Surface>) -> SmartObject {
    SmartObject {
        contents_id: photocraft_doc::SmartContentsId::fresh(),
        source,
        transform,
        smart_filters: vec![],
        cache,
        psd_raw: None,
        filters_enabled: true,
        filter_mask: None,
        warp: None,
        stack_mode: None,
        perspective: None,
    }
}

fn base(mode: ColorMode, depth: SampleType) -> Document {
    gen_doc(mode, depth, Features::PIXELS)
}

/// The nested document of a .pcraft smart object (optionally holding a smart object itself).
fn nested_pcraft(inner: bool) -> Vec<u8> {
    let mut d = Document::new("nested", Size::new(12, 8), ColorMode::Rgb, SampleType::U8);
    d.layers.push(raster("a", PixelFormat::RGBA8, Rect::new(1, 1, 10, 7), 21, true));
    if inner {
        let src = SmartSource::Embedded { file_name: "inner.png".into(), bytes: Arc::new(png_bytes(4, 3, 5)) };
        let cache = pattern(PixelFormat::RGBA8, Rect::new(2, 2, 6, 5), 3, true);
        d.layers.push(Layer::new("inner smart", LayerContent::Smart(smart(src, Affine { m: [1.0, 0.0, 0.0, 1.0, 2.0, 2.0] }, Some(cache)))));
    }
    photocraft_format::save_to_bytes(&d, &photocraft_format::SaveOptions::default()).expect("pcraft")
}

fn docs() -> Vec<(String, Document)> {
    let mut v: Vec<(String, Document)> = Vec::new();
    for mode in [ColorMode::Rgb, ColorMode::Grayscale, ColorMode::Cmyk, ColorMode::Lab] {
        for depth in [SampleType::U8, SampleType::U16, SampleType::F32] {
            v.push((format!("gen-{mode:?}-{depth:?}"), gen_doc(mode, depth, Features::ALL)));
        }
    }
    v.push(("pix-Rgb-U8".into(), base(ColorMode::Rgb, SampleType::U8)));
    v.push(("empty".into(), Document::new("e", Size::new(5, 3), ColorMode::Rgb, SampleType::U8)));
    v.push(("empty-16".into(), Document::new("e", Size::new(5, 3), ColorMode::Rgb, SampleType::U16)));

    // Effects: typed items, group effects (lfxs), a reference point, master switch off.
    for depth in [SampleType::U8, SampleType::F32] {
        let mut d = base(ColorMode::Rgb, depth);
        d.global_light = GlobalLight { angle: 45.0, altitude: 25.0 };
        d.layers[1].effects.items = vec![
            Effect::default_drop_shadow(),
            Effect::Stroke(StrokeFx {
                common: FxCommon::new(BlendMode::Normal, 1.0),
                size: 3.0,
                position: StrokePosition::Center,
                paint: FxPaint::Color(Color::rgb(0.0, 1.0, 0.0)),
            }),
        ];
        d.layers[1].effects.reference = Some((2.5, -1.0));
        d.layers[2].effects.items = vec![Effect::ColorOverlay { common: FxCommon::new(BlendMode::Multiply, 0.5), color: Color::rgb(1.0, 0.0, 0.0) }];
        d.layers[2].effects.enabled = false;
        if let Some(n) = d.layers.iter().position(|l| l.is_group()) {
            d.layers[n].effects.items.push(Effect::ColorOverlay { common: FxCommon::new(BlendMode::Normal, 1.0), color: Color::rgb(0.0, 0.0, 1.0) });
        }
        v.push((format!("effects-{depth:?}"), d));
    }

    // Fills: gradient, pattern (with the document pattern), a valid fill cache.
    {
        let mut d = base(ColorMode::Rgb, SampleType::U8);
        d.patterns.push(Pattern { id: "pat-1".into(), name: "Dots".into(), width: 4, height: 4, surface: pattern(PixelFormat::RGBA8, Rect::new(0, 0, 4, 4), 9, true) });
        let grad = Fill::Gradient {
            stops: vec![(0.0, Color::rgb(1.0, 0.0, 0.0)), (1.0, Color::rgb(0.0, 0.0, 1.0))],
            angle: 30.0,
            scale: 1.0,
            style: GradientStyle::Linear,
            reverse: false,
            opacity_stops: vec![(0.0, 1.0), (1.0, 0.5)],
            midpoints: vec![0.4],
            offset: (0.1, 0.0),
            dither: false,
            align: true,
        };
        d.layers.push(Layer::new("Gradient", LayerContent::Fill(grad)));
        d.layers.push(Layer::new(
            "Pattern",
            LayerContent::Fill(Fill::Pattern { name: "Dots".into(), scale: 1.0, id: "pat-1".into(), angle: 0.0, link: true, phase: (0.0, 0.0) }),
        ));
        let solid = Fill::Solid(Color::rgb(0.2, 0.4, 0.6));
        let mut l = Layer::new("Cached", LayerContent::Fill(solid.clone()));
        l.fill_cache = Some(FillCache { fill: solid, surface: pattern(PixelFormat::RGBA8, Rect::new(0, 0, 24, 16), 77, false) });
        d.layers.push(l);
        let mut stale = Layer::new("Stale cache", LayerContent::Fill(Fill::Solid(Color::rgb(0.9, 0.1, 0.1))));
        stale.fill_cache = Some(FillCache { fill: Fill::Solid(Color::BLACK), surface: pattern(PixelFormat::RGBA8, Rect::new(0, 0, 4, 4), 1, false) });
        d.layers.push(stale);
        v.push(("fills".into(), d));
    }

    // Masks: vector masks with density/feather, parameters only, user + vector.
    for depth in [SampleType::U8, SampleType::U16] {
        let mut d = base(ColorMode::Rgb, depth);
        let fmt = d.pixel_format();
        let mut a = raster("Vector", fmt, Rect::new(0, 0, 20, 12), 31, true);
        let mut vm = VectorMask::new(rect_path(2.0, 2.0, 15.5, 10.25));
        vm.density = g(200);
        vm.feather = 1.5;
        a.vector_mask = Some(vm);
        d.layers.push(a);
        let mut b = raster("Both", fmt, Rect::new(3, 1, 22, 15), 32, true);
        b.mask = Some(LayerMask { surface: mask_surface(depth, Rect::new(4, 2, 12, 9), 0.0, 5), enabled: true, linked: false, density: g(100), feather: 0.0 });
        let mut vm2 = VectorMask::new(rect_path(1.0, 1.0, 9.0, 9.0));
        vm2.enabled = false;
        vm2.linked = false;
        b.vector_mask = Some(vm2);
        d.layers.push(b);
        let mut c = raster("Plain vector", fmt, Rect::new(0, 0, 4, 4), 33, true);
        c.vector_mask = Some(VectorMask::new(rect_path(0.0, 0.0, 3.0, 3.0)));
        d.layers.push(c);
        v.push((format!("masks-{depth:?}"), d));
    }

    // Shapes and text (generated TySh).
    {
        let mut d = base(ColorMode::Rgb, SampleType::U8);
        let cache = pattern(PixelFormat::RGBA8, Rect::new(2, 2, 12, 9), 40, true);
        let sh = ShapeLayer {
            path: rect_path(2.0, 2.0, 12.0, 9.0),
            fill: Some(Fill::Solid(Color::rgb(0.1, 0.6, 0.3))),
            stroke: Some(ShapeStroke { width: 2.0, ..Default::default() }),
            live: Some(LiveShape::Rect { rect: [2.0, 2.0, 10.0, 7.0], radii: [0.0; 4] }),
            cache: Some(cache.clone()),
            psd_raw: None,
        };
        d.layers.push(Layer::new("Shape", LayerContent::Shape(sh)));
        let nofill = ShapeLayer { path: rect_path(1.0, 1.0, 5.0, 5.0), fill: None, cache: Some(cache.clone()), ..Default::default() };
        d.layers.push(Layer::new("No fill", LayerContent::Shape(nofill)));
        let t = TextLayer { text: "Hello PSD".into(), size_pt: 18.0, cache: Some(cache), ..TextLayer::default() };
        d.layers.push(Layer::new("Text", LayerContent::Text(t)));
        v.push(("shapes-text".into(), d));
    }

    // Adjustments the other docs lack: tetrahedral Color Lookup, legacy 18-byte Photo Filter.
    {
        let mut d = base(ColorMode::Rgb, SampleType::U8);
        let pf = Adjustment::PhotoFilter { color: [60000u16, 30000, 0].map(|x| x as f32 / 65535.0), density: 0.14, preserve_luminosity: true };
        let mut legacy = photocraft_io::export(&{
            let mut x = Document::new("x", Size::new(2, 2), ColorMode::Rgb, SampleType::U8);
            x.layers.push(Layer::new("pf", LayerContent::Adjustment(pf.clone())));
            x
        }, "psd", &photocraft_io::ExportOptions::default())
        .ok()
        .and_then(|r| PsdFile::from_bytes(&r.bytes).ok())
        .and_then(|f| f.layers().iter().find_map(|l| l.block(b"phfl").map(|b| b.data.clone())))
        .unwrap_or_default();
        legacy.truncate(18);
        let mut l = Layer::new("Legacy filter", LayerContent::Adjustment(pf));
        l.psd_blocks = vec![(*b"phfl", Arc::new(legacy))];
        d.layers.push(l);
        v.push(("adjust-extra".into(), d));
    }

    // Smart objects: embedded image with filters, filter mask, warp; a second instance of the
    // same source; perspective; stack mode; undecodable and unreadable sources; nested .pcraft.
    for depth in [SampleType::U8, SampleType::U16, SampleType::F32] {
        let mut d = base(ColorMode::Rgb, depth);
        let fmt = d.pixel_format();
        let png = Arc::new(png_bytes(8, 6, 11));
        let src = || SmartSource::Embedded { file_name: "src.png".into(), bytes: png.clone() };
        let cache = pattern(fmt, Rect::new(3, 2, 19, 14), 50, true);
        let mut a = smart(src(), Affine { m: [2.0, 0.0, 0.0, 2.0, 3.0, 2.0] }, Some(cache.clone()));
        a.smart_filters = vec![
            sf("filter.blur.gaussianBlur", json!({"radius": 2.0})),
            sf("image.adjustments.levels", json!({"inBlack": 10, "inWhite": 240, "gamma": 1.1})),
            sf("filter.distort.twirl", json!({"angle": 50})),
        ];
        a.smart_filters[1].blend = BlendMode::Screen;
        a.smart_filters[1].opacity = 0.5;
        a.filter_mask = Some(LayerMask { surface: mask_surface(depth, Rect::new(5, 4, 12, 10), 1.0, 7), enabled: true, linked: true, density: 1.0, feather: 0.0 });
        d.layers.push(Layer::new("Smart filtered", LayerContent::Smart(a)));
        let mut b = smart(src(), Affine { m: [1.0, 0.0, 0.0, 1.0, -2.0, 1.0] }, Some(pattern(fmt, Rect::new(0, 1, 6, 7), 51, true)));
        b.warp = Some(photocraft_geom::warp::Warp::preset(photocraft_geom::warp::WarpStyle::Arc, 30.0, [0.0, 0.0, 8.0, 6.0]));
        d.layers.push(Layer::new("Smart warped", LayerContent::Smart(b)));
        if depth == SampleType::U8 {
            let mut c = smart(src(), Affine::IDENTITY, Some(cache.clone()));
            c.perspective = Some([1.2, 0.1, 2.0, 0.05, 0.9, 1.0, 0.001, 0.002, 1.0]);
            c.smart_filters = vec![sf("filter.noise.median", json!({"radius": 1.0}))];
            d.layers.push(Layer::new("Smart perspective", LayerContent::Smart(c)));
            let mut s = smart(src(), Affine::IDENTITY, Some(cache.clone()));
            s.stack_mode = Some(StackMode::Median);
            s.smart_filters = vec![sf("filter.other.highPass", json!({"radius": 3.0}))];
            d.layers.push(Layer::new("Smart stack", LayerContent::Smart(s)));
            let g = SmartSource::Embedded { file_name: "art.ai".into(), bytes: Arc::new(b"%PDF-1.4 not really".to_vec()) };
            d.layers.push(Layer::new("Smart undecodable", LayerContent::Smart(smart(g, Affine { m: [0.5, 0.0, 0.0, 0.5, 1.0, 1.0] }, Some(cache.clone())))));
            let gone = SmartSource::Linked { path: "/nonexistent/photocraft-i3/missing.png".into() };
            d.layers.push(Layer::new("Smart missing", LayerContent::Smart(smart(gone, Affine::IDENTITY, Some(cache.clone())))));
            let nested = SmartSource::Embedded { file_name: "nested.pcraft".into(), bytes: Arc::new(nested_pcraft(true)) };
            d.layers.push(Layer::new("Smart nested", LayerContent::Smart(smart(nested, Affine { m: [1.0, 0.0, 0.0, 1.0, 4.0, 3.0] }, Some(cache.clone())))));
            let nested2 = SmartSource::Embedded { file_name: "flat.pcraft".into(), bytes: Arc::new(nested_pcraft(false)) };
            let mut n2 = smart(nested2, Affine::IDENTITY, Some(cache.clone()));
            n2.smart_filters = vec![sf("filter.blur.boxBlur", json!({"radius": 1.0}))];
            n2.filters_enabled = false;
            d.layers.push(Layer::new("Smart nested filtered", LayerContent::Smart(n2)));
        }
        v.push((format!("smart-{depth:?}"), d));
    }

    // Artboards, comps, slices, notes, measurement scale, patterns.
    {
        let mut d = base(ColorMode::Rgb, SampleType::U8);
        let fmt = d.pixel_format();
        let mut board = Layer::group("Board", vec![raster("On board", fmt, Rect::new(2, 2, 8, 8), 60, true)]);
        if let LayerContent::Group(gr) = &mut board.content {
            gr.artboard = Some(Artboard::new(Rect::new(1, 1, 12, 10)));
        }
        d.layers.push(board);
        d.guides.vertical.push(3.0);
        d.guides.horizontal.push(4.5);
        d.layer_comps = vec![
            LayerComp { id: 7, name: "First".into(), comment: "c".into(), apply_visibility: true, apply_position: true, apply_appearance: false, states: comps::capture_states(&d) },
            LayerComp { id: 9, name: "Second".into(), comment: String::new(), apply_visibility: true, apply_position: false, apply_appearance: true, states: comps::capture_states(&d) },
        ];
        d.last_applied_comp = Some(9);
        d.slices.group_name = "doc".into();
        d.slices.list.push(Slice { id: 1, rect: Rect::new(0, 0, 10, 8), name: "s1".into(), ..Default::default() });
        d.notes.push(Note { author: "A".into(), text: "note".into(), position: [3.0, 4.0], popup: [3.0, 4.0, 50.0, 40.0], ..Default::default() });
        d.measurement.scale = MeasurementScale { pixel_length: 10.0, logical_length: 2.5, units: "cm".into() };
        d.patterns.push(Pattern { id: "pat-2".into(), name: "Grid".into(), width: 3, height: 2, surface: pattern(PixelFormat::RGBA8, Rect::new(0, 0, 3, 2), 8, false) });
        v.push(("boards-comps".into(), d));
    }

    // Channels, Quick Mask, paths, link groups, psd ids, preserved blocks and resources.
    {
        let mut d = base(ColorMode::Rgb, SampleType::U16);
        let mut a = AlphaChannel::new("Spot", mask_surface(SampleType::U16, Rect::new(0, 0, 24, 16), 0.0, 4));
        a.spot = Some((Color::rgb(0.0, 0.5, 1.0), 0.7));
        d.channels.push(a);
        d.channels.push(AlphaChannel::new("Alpha \u{e9}", mask_surface(SampleType::U16, Rect::new(2, 2, 9, 9), 0.0, 6)));
        d.quick_mask = Some(AlphaChannel::new("Quick Mask", mask_surface(SampleType::U16, Rect::new(1, 1, 5, 5), 0.0, 2)));
        d.paths.push(NamedPath { name: "Path 1".into(), path: rect_path(1.0, 1.0, 10.0, 10.0), psd_raw: None });
        d.paths.push(NamedPath { name: "Clip".into(), path: rect_path(2.0, 3.0, 20.0, 12.0), psd_raw: None });
        d.work_path = Some(rect_path(0.5, 0.5, 4.5, 4.5));
        d.clipping_path = Some(ClippingPath { name: "Clip".into(), flatness: 1.5 });
        d.metadata.psd_resources.push((2999, String::new(), Arc::new(vec![4, b'O', b'l', b'd', b'!', 0, 1, 0, 0])));
        d.metadata.psd_resources.push((4001, "named".into(), Arc::new(vec![1, 2, 3])));
        d.layers[1].link_group = Some(3);
        d.layers[2].link_group = Some(3);
        d.layers[3].link_group = Some(70_000);
        d.layers[0].psd_id = Some(5);
        d.layers[1].psd_id = Some(5);
        d.layers[2].psd_id = Some(2);
        d.layers[2].psd_blocks = vec![
            (*b"zzzz", Arc::new(vec![1, 2, 3])),
            (*b"luni", Arc::new(vec![0; 8])),
            (*b"fxrp", Arc::new(vec![0; 16])),
            (*b"brst", Arc::new(vec![0, 0, 0, 1])),
        ];
        d.layers[2].excluded_channels = 0b10;
        v.push(("channels-paths".into(), d));
    }
    {
        let mut d = base(ColorMode::Rgb, SampleType::U8);
        for i in 0..60 {
            d.channels.push(AlphaChannel::new(format!("A{i}"), Surface::new(PixelFormat::GRAY8)));
        }
        d.quick_mask = Some(AlphaChannel::new("Quick Mask", Surface::new(PixelFormat::GRAY8)));
        v.push(("channels-60".into(), d));
    }

    // Merged-image edge cases: almost opaque (matte re-render), translucent, other modes.
    {
        let mut d = Document::new("t", Size::new(6, 4), ColorMode::Rgb, SampleType::U8);
        let mut s = Surface::new(PixelFormat::RGBA8);
        s.write_region(Rect::new(0, 0, 6, 4), &[0.8f32, 0.2, 0.1, 0.9995].repeat(24));
        d.layers.push(Layer::new("near", LayerContent::Raster(s)));
        v.push(("near-opaque".into(), d));
        let mut d = Document::new("t", Size::new(6, 4), ColorMode::Cmyk, SampleType::U16);
        let mut s = Surface::new(PixelFormat::new(ColorMode::Cmyk, SampleType::U16, true));
        s.write_region(Rect::new(1, 0, 5, 4), &[0.1f32, 0.5, 0.2, 0.0, 0.5].repeat(16));
        d.layers.push(Layer::new("half", LayerContent::Raster(s)));
        v.push(("translucent-cmyk16".into(), d));
        let mut d = gen_doc(ColorMode::Lab, SampleType::U16, Features::PIXELS);
        d.layers[1].opacity = 0.5;
        v.push(("lab16".into(), d));
    }
    for mode in [ColorMode::Indexed, ColorMode::Duotone, ColorMode::Bitmap, ColorMode::Multichannel] {
        let mut d = Document::new("m", Size::new(5, 3), mode, SampleType::U8);
        let fmt = d.pixel_format();
        d.layers.push(raster("px", fmt, Rect::new(0, 0, 5, 3), 3, false));
        if mode == ColorMode::Multichannel {
            d.layers.clear();
            d.channels.push(AlphaChannel::new("Ink", mask_surface(SampleType::U8, Rect::new(0, 0, 5, 3), 0.0, 1)));
        }
        v.push((format!("mode-{mode:?}"), d));
    }
    v.push(("big".into(), {
        let mut d = Document::new("b", Size::new(30_001, 2), ColorMode::Grayscale, SampleType::U8);
        d.layers.push(raster("px", PixelFormat::GRAYA8, Rect::new(29_990, 0, 30_001, 2), 3, true));
        d
    }));
    v
}

// ---------------------------------------------------------------------------------------------
// smart-object vectors
// ---------------------------------------------------------------------------------------------

fn filter_s(f: &SmartFilter) -> String {
    format!("{}|{}|{:?}|{:08x}|{}", f.command, serde_json::to_string(&f.params).unwrap_or_default(), f.blend, f.opacity.to_bits(), u8::from(f.visible))
}

fn filters() -> Vec<(String, SmartFilter)> {
    let mut v = vec![
        ("gauss", sf("filter.blur.gaussianBlur", json!({"radius": 4.0}))),
        ("box", sf("filter.blur.boxBlur", json!({"radius": 5.0}))),
        ("motion", sf("filter.blur.motionBlur", json!({"angle": -20, "distance": 10.0}))),
        ("unsharp", sf("filter.sharpen.unsharpMask", json!({"amount": 150.0, "radius": 2.0, "threshold": 0}))),
        ("noise", sf("filter.noise.addNoise", json!({"amount": 12.0, "distribution": "uniform", "monochromatic": false, "seed": 1234}))),
        ("noise-g", sf("filter.noise.addNoise", json!({"amount": 3.5, "distribution": "gaussian", "monochromatic": true}))),
        ("median", sf("filter.noise.median", json!({"radius": 3.0}))),
        ("highpass", sf("filter.other.highPass", json!({"radius": 3.0}))),
        ("max", sf("filter.other.maximum", json!({"radius": 2.0}))),
        ("min", sf("filter.other.minimum", json!({"radius": 2.0, "preserve": "squareness"}))),
        ("min-round", sf("filter.other.minimum", json!({"radius": 2.0, "preserve": "roundness"}))),
        ("mosaic", sf("filter.pixelate.mosaic", json!({"cellSize": 8.0}))),
        ("emboss", sf("filter.stylize.emboss", json!({"angle": 135, "height": 3, "amount": 100}))),
        ("emboss-defaults", sf("filter.stylize.emboss", json!({}))),
        ("levels", sf("image.adjustments.levels", json!({"inBlack": 20, "inWhite": 220, "gamma": 1.4, "red": {"inBlack": 0, "inWhite": 255, "gamma": 0.8}}))),
        ("levels-out", sf("image.adjustments.levels", json!({"outBlack": 5, "blue": {"outWhite": 250}, "layer": 3, "__x": 1}))),
        ("levels-bad", sf("image.adjustments.levels", json!({"cyan": {"gamma": 2.0}}))),
        ("levels-notobj", sf("image.adjustments.levels", json!([1, 2]))),
        ("curves", sf("image.adjustments.curves", json!({"points": [[0.0, 0.0], [64.0, 40.0], [255.0, 255.0]], "blue": [[0.0, 20.0], [255.0, 235.0]]}))),
        ("curves-bad", sf("image.adjustments.curves", json!({"points": [[0.0], [255.0, 255.0]]}))),
        ("sh", sf("image.adjustments.shadowsHighlights", json!({}))),
        ("sh-meta", sf("image.adjustments.shadowsHighlights", json!({"layer": 1}))),
        ("sh-set", sf("image.adjustments.shadowsHighlights", json!({"shadowAmount": 50}))),
        ("twirl", sf("filter.distort.twirl", json!({"angle": 50}))),
        ("unsup-bad", sf(smart_map::UNSUPPORTED_FILTER, json!({"psd": "zz"}))),
        ("unsup-missing", sf(smart_map::UNSUPPORTED_FILTER, json!({}))),
        ("unsup-garbage", sf(smart_map::UNSUPPORTED_FILTER, json!({"psd": "00000001"}))),
        ("nan", sf("filter.blur.gaussianBlur", json!({"radius": "x"}))),
    ]
    .into_iter()
    .map(|(n, f)| (n.to_string(), f))
    .collect::<Vec<_>>();
    let mut o = sf("filter.blur.gaussianBlur", json!({"radius": 1.5}));
    o.blend = BlendMode::Overlay;
    o.opacity = 0.75;
    o.visible = false;
    v.push(("gauss-blend".into(), o));
    let mut o = sf("filter.noise.median", json!({"radius": 2.0}));
    o.blend = BlendMode::Hue;
    o.opacity = 1.7;
    v.push(("median-clamp".into(), o));
    let mut o = sf("filter.other.highPass", json!({"radius": 2.0}));
    o.blend = BlendMode::Divide;
    o.opacity = f32::NAN;
    v.push(("highpass-nan".into(), o));
    v
}

fn raw_items() -> Vec<(String, photocraft_psd::descriptor::Descriptor)> {
    use photocraft_psd::descriptor::{Descriptor, Id, UnicodeString, Value};
    let bo = |op: f64, md: &str| {
        Value::Descriptor(
            Descriptor::new("blendOptions")
                .with("Opct", Value::UnitFloat { unit: *b"#Prc", value: op })
                .with("Md  ", Value::Enumerated { type_id: Id::new("BlnM"), value: Id::new(md) }),
        )
    };
    vec![
        (
            "twirl".into(),
            Descriptor::new("filterFX")
                .with("Nm  ", Value::Text(UnicodeString::new_nul("Twi&rl...")))
                .with("blendOptions", bo(80.0, "Scrn"))
                .with("enab", Value::Boolean(false))
                .with("Fltr", Value::Descriptor(Descriptor::new("Twrl").with("Angl", Value::Integer(50))))
                .with("filterID", Value::Integer(i32::from_be_bytes(*b"Twrl"))),
        ),
        ("gsnb-noopts".into(), Descriptor::new("filterFX").with("Fltr", Value::Descriptor(Descriptor::new("GsnB"))).with("filterID", Value::Integer(i32::from_be_bytes(*b"GsnB")))),
        ("id-only".into(), Descriptor::new("filterFX").with("filterID", Value::Integer(781)).with("blendOptions", bo(250.0, "multiply"))),
        ("id-gauss".into(), Descriptor::new("filterFX").with("filterID", Value::Integer(i32::from_be_bytes(*b"GsnB")))),
        (
            "adns-bad".into(),
            Descriptor::new("filterFX").with(
                "Fltr",
                Value::Descriptor(
                    Descriptor::new("AdNs").with("Dstr", Value::Enumerated { type_id: Id::new("Dstr"), value: Id::new("Xxxx") }).with("Nose", Value::Double(3.0)),
                ),
            ),
        ),
        ("empty".into(), Descriptor::new("filterFX")),
    ]
}

fn spec<'a>(idnt: &'a str, placed: &'a str, t: Affine, size: (f64, f64), warp: Option<&'a photocraft_geom::warp::Warp>, fx: Option<photocraft_psd::descriptor::Descriptor>) -> PlacedSpec<'a> {
    PlacedSpec { idnt, placed, transform: t, perspective: None, size, dpi: 72.0, warp, filter_fx: fx }
}

fn affine_s(a: &Affine) -> String {
    a.m.iter().map(|v| format!("{:016x}", v.to_bits())).collect::<Vec<_>>().join(",")
}

fn mask_s(m: &Option<LayerMask>) -> String {
    match m {
        None => "none".into(),
        Some(m) => {
            let b = m.surface.content_bounds();
            format!(
                "{},{},{},{}:{}:{}:{}",
                b.x0,
                b.y0,
                b.x1,
                b.y1,
                h16(&m.surface.to_interleaved(b)),
                u8::from(m.enabled),
                u8::from(m.linked)
            )
        }
    }
}

fn fx_item_bytes(it: &FilterEffectsItem) -> String {
    let fx = FilterEffects { version: 3, items: vec![it.clone()] };
    let raw = fx.to_bytes();
    let mut any = false;
    let mut n = it.clone();
    if let (Ok((w, h)), Ok(d)) = (n.rect.size(), u16::try_from(n.depth)) {
        for s in n.slots.iter_mut().flatten() {
            *s = raw_plane(s, w, h, d, &mut any);
        }
        if let Some((r, p)) = &mut n.mask {
            if let Ok((mw, mh)) = r.size() {
                *p = raw_plane(p, mw, mh, d, &mut any);
            }
        }
    }
    let norm = FilterEffects { version: 3, items: vec![n] }.to_bytes();
    format!("{} {} {} {}", raw.len(), h16(&raw), u8::from(any), h16(&norm))
}

fn smart_vectors() {
    use photocraft_psd::descriptor::Descriptor;
    for (name, f) in filters() {
        match smart_map::item_for_filter(&f) {
            Ok(d) => {
                let bytes = d.to_bytes();
                println!("sfw\t{name}\tok {}", hexs(&bytes));
                match Descriptor::from_bytes(&bytes) {
                    Ok(back) => println!("sfr\t{name}\t{}", filter_s(&smart_map::filter_from_item(&back))),
                    Err(e) => println!("sfr\t{name}\terr {e}"),
                }
            }
            Err(e) => println!("sfw\t{name}\terr {e:?}"),
        }
    }
    for (name, d) in raw_items() {
        println!("sfr\traw-{name}\t{}\t{}", hexs(&d.to_bytes()), filter_s(&smart_map::filter_from_item(&d)));
    }
    // Stacks.
    let all: Vec<SmartFilter> = filters().into_iter().map(|(_, f)| f).collect();
    let stacks = vec![
        ("all", FilterStack { enabled: true, filters: all.clone(), mask_enabled: true, mask_linked: false }),
        ("off", FilterStack { enabled: false, filters: all[..3].to_vec(), mask_enabled: false, mask_linked: true }),
        ("none", FilterStack::default()),
    ];
    for (name, st) in &stacks {
        let mut w = Vec::new();
        let d = smart_map::filter_fx(st, "L \u{e9}", &mut w);
        println!("sff\t{name}\t{}\t{}", hexs(&d.to_bytes()), warn_s(&w));
        let back = smart_map::filter_stack(&d);
        println!(
            "sffr\t{name}\t{}:{}:{}:{}",
            u8::from(back.enabled),
            u8::from(back.mask_enabled),
            u8::from(back.mask_linked),
            back.filters.iter().map(filter_s).collect::<Vec<_>>().join(";")
        );
    }
    // Placed-layer blocks.
    use photocraft_geom::warp::{BezierMesh, Warp, WarpStyle};
    let bounds = [0.0, 0.0, 60.0, 30.0];
    let mut custom = BezierMesh::identity(bounds, 1, 1);
    custom.points[15] = [67.0, 30.0];
    let mut cw = Warp::preset(WarpStyle::Custom, 0.0, bounds);
    cw.mesh = Some(custom);
    let mut multi = Warp::preset(WarpStyle::Custom, 0.0, bounds);
    multi.mesh = Some(BezierMesh::identity(bounds, 2, 2));
    let mut arc = Warp::preset(WarpStyle::Arc, 40.0, bounds);
    arc.vertical = true;
    arc.h_distort = 10.0;
    let t = Affine { m: [0.5, 0.1, -0.1, 0.5, 10.0, 20.0] };
    let fx = smart_map::filter_fx(&stacks[1].1, "L", &mut Vec::new());
    let long_id = "x".repeat(300);
    let cases: Vec<(&str, PlacedSpec)> = vec![
        ("plain", spec("id-1", "pl-2", t, (64.0, 32.0), None, None)),
        ("fx", spec("id-1", "pl-2", t, (64.0, 32.0), None, Some(fx.clone()))),
        ("arc", spec("i", "p", Affine::IDENTITY, (60.0, 30.0), Some(&arc), None)),
        ("custom", spec("i", "p", Affine::IDENTITY, (60.0, 30.0), Some(&cw), None)),
        ("multi", spec("i", "p", Affine::IDENTITY, (60.0, 30.0), Some(&multi), None)),
        ("long-id", spec(&long_id, "p", Affine::IDENTITY, (1.0, 1.0), None, None)),
        ("persp", PlacedSpec { perspective: Some([1.2, 0.1, 2.0, 0.05, 0.9, 1.0, 0.001, 0.002, 1.0]), ..spec("i", "p", Affine::IDENTITY, (20.0, 10.0), None, None) }),
    ];
    for (name, s) in &cases {
        let mut w = Vec::new();
        let sold = smart_map::sold_bytes(None, s, &mut w);
        println!("sold\t{name}\t{}\t{}", hexs(&sold), warn_s(&w));
        let mut w = Vec::new();
        let plld = smart_map::plld_bytes(s, &mut w);
        println!("plld\t{name}\t{}\t{}", hexs(&plld), warn_s(&w));
        let p = smart_map::parse_sold(&sold);
        println!(
            "psold\t{name}\t{}",
            match &p {
                None => "none".into(),
                Some(p) => format!(
                    "{}|{}|{}|{}",
                    p.idnt,
                    p.placed,
                    affine_s(&p.transform),
                    p.stack.as_ref().map_or("-".into(), |s| format!("{}:{}", u8::from(s.enabled), s.filters.iter().map(filter_s).collect::<Vec<_>>().join(";")))
                ),
            }
        );
        // With a template: the same, moved, and with an extra key.
        if let Some(p) = p {
            let mut tmpl = p.descriptor.clone();
            tmpl.items.push((photocraft_psd::descriptor::Id::new("extraKey"), photocraft_psd::descriptor::Value::Integer(7)));
            let mut w = Vec::new();
            let same = smart_map::sold_bytes(Some(&tmpl), &PlacedSpec { filter_fx: None, ..s.clone() }, &mut w);
            println!("sold\t{name}-tmpl\t{}\t{}", hexs(&same), warn_s(&w));
            let moved = PlacedSpec { transform: Affine { m: [1.0, 0.0, 0.0, 1.0, 0.5, 0.0] }, filter_fx: Some(fx.clone()), ..s.clone() };
            let mut w = Vec::new();
            let m = smart_map::sold_bytes(Some(&tmpl), &moved, &mut w);
            println!("sold\t{name}-moved\t{}\t{}", hexs(&m), warn_s(&w));
        }
    }
    for (name, data) in [("short", vec![b's', b'o', b'L']), ("plcl", b"plcL\0\0\0\x03".to_vec()), ("bad-desc", b"soLD\0\0\0\x04\0\0\0\x10junk".to_vec())] {
        println!("psold\t{name}\t{}\t{}", hexs(&data), smart_map::parse_sold(&data).map_or("none".to_string(), |p| p.idnt));
    }
    // Filter masks and caches.
    for sample in [SampleType::U8, SampleType::U16, SampleType::F32] {
        let fmt = PixelFormat::new(ColorMode::Grayscale, sample, false);
        let mut ms = Surface::with_default(fmt, &[1.0]);
        ms.write_region(Rect::new(2, 1, 6, 4), &(0..12).map(|i| (i * 20) as f32 / 255.0).collect::<Vec<_>>());
        let lm = LayerMask { surface: ms, enabled: true, linked: false, density: 1.0, feather: 0.0 };
        for doc_mode in [ColorMode::Rgb, ColorMode::Cmyk, ColorMode::Lab] {
            let doc_fmt = PixelFormat::new(doc_mode, sample, true);
            let src = pattern(doc_fmt, Rect::new(1, 1, 7, 5), 12, true);
            for (mname, m) in [("mask", Some(&lm)), ("nomask", None)] {
                let name = format!("{sample:?}-{doc_mode:?}-{mname}");
                match smart_map::feid_item("pl-x", &src, m, Rect::new(0, 0, 9, 6), doc_fmt) {
                    None => println!("feid\t{name}\tnone"),
                    Some(it) => {
                        println!("feid\t{name}\t{}", fx_item_bytes(&it));
                        let st = FilterStack { mask_enabled: false, mask_linked: true, ..Default::default() };
                        let r = smart_map::mask_from_item(&it, sample, &st);
                        println!("mask\t{name}\t{}", match r {
                            Ok(m) => format!("ok {}", mask_s(&m)),
                            Err(e) => format!("err {e:?}"),
                        });
                        let other = if sample == SampleType::U8 { SampleType::U16 } else { SampleType::U8 };
                        println!("mask\t{name}-depth\t{}", match smart_map::mask_from_item(&it, other, &st) {
                            Ok(m) => format!("ok {}", mask_s(&m)),
                            Err(e) => format!("err {e:?}"),
                        });
                    }
                }
            }
            let empty = Surface::new(doc_fmt);
            if smart_map::feid_item("e", &empty, None, Rect::new(0, 0, 2, 2), doc_fmt).is_none() {
                println!("feid\t{sample:?}-{doc_mode:?}-empty\tnone");
            }
        }
    }
    for seed in [&b""[..], b"x", b"y", b"some longer seed:42"] {
        println!("uuid\t{}\t{}", hexs(seed), smart_map::uuid_from(seed));
    }
}

// ---------------------------------------------------------------------------------------------

fn export_lines(name: &str, d: &Document) {
    for (variant, opts) in [
        ("d", PsdExportOptions::default()),
        ("b", PsdExportOptions { force_psb: true, ..Default::default() }),
        ("t", PsdExportOptions { force_psb: false, merged_matte: false, merged_compression: Compression::Raw }),
    ] {
        let (f, w) = document_to_psd_with(d, &opts);
        let (r, o) = psd_result(&f, &w);
        println!("psd\t{name}\t{variant}\t{r}\t{o}");
    }
    let (f, _) = document_to_psd_with(d, &PsdExportOptions::default());
    let Ok(bytes) = f.to_bytes() else { return };
    let Ok(file) = PsdFile::from_bytes(&bytes) else {
        println!("rt\t{name}\tunparsable\t-");
        return;
    };
    let (back, iw) = psd_to_document(&file);
    let (f2, w2) = document_to_psd_with(&back, &PsdExportOptions::default());
    let (r, o) = psd_result(&f2, &w2);
    println!("rt\t{name}\t{r} I{iw:?}\t{o}");
    let Ok(b2) = f2.to_bytes() else { return };
    let Ok(file2) = PsdFile::from_bytes(&b2) else { return };
    let (back2, _) = psd_to_document(&file2);
    let (f3, w3) = document_to_psd_with(&back2, &PsdExportOptions::default());
    let (r3, _) = psd_result(&f3, &w3);
    println!("rt2\t{name}\t{r3}");
}

fn corpus() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../corpus");
    for dir in ["psd", "psd-tools", "photoshop"] {
        let mut files = Vec::new();
        collect(&root.join(dir), &mut files);
        files.sort();
        for p in files {
            let rel = p.strip_prefix(&root).map(|r| r.display().to_string()).unwrap_or_default();
            let Ok(bytes) = std::fs::read(&p) else { continue };
            let name = p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            let r = match photocraft_io::import(&name, &bytes) {
                Err(e) => format!("imperr {:?}", e.to_string()),
                Ok(r) => {
                    let (f, w) = document_to_psd_with(&r.document, &PsdExportOptions::default());
                    psd_result(&f, &w).0
                }
            };
            println!("corp\t{rel}\t{r}");
        }
    }
}

fn collect(dir: &std::path::Path, out: &mut Vec<std::path::PathBuf>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            collect(&p, out);
        } else if p.extension().is_some_and(|x| x.eq_ignore_ascii_case("psd") || x.eq_ignore_ascii_case("psb")) {
            out.push(p);
        }
    }
}

pub fn run() {
    for (name, d) in docs() {
        let bytes = photocraft_format::save_to_bytes(&d, &photocraft_format::SaveOptions::default()).expect("pcraft");
        let d = photocraft_format::load_from_bytes(&bytes).expect("reload");
        println!("doc\t{name}\t{}", b64(&bytes));
        export_lines(&name, &d);
    }
    smart_vectors();
    corpus();
}
