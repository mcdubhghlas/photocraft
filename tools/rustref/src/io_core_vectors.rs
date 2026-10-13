//! I1 io flat/pixels/raw/linked/TIFF layers/channels reference vectors (`rustref io-core-vectors`),
//! checked by tests/io/test_core_parity.c. Lines are tab-separated; the first field names the kind.
//!
//!   px-il N S HASH | px-dl N S HASH       pixels::interleave / deinterleave (4 planes, 1 missing)
//!   px-lab HASH                           pixels::lab16_chroma over every u16, both directions
//!   px-be S BITS HEX                      pixels::encode_be of the f32 with BITS
//!   px-matte HASH                         matte/unmatte over a value grid
//!   cm-di CASE HEX | cm-apply CASE RES    channel_map::display_info / apply_display_info (copied)
//!   cm-color SPACE C0 C1 C2 C3 RGB        channel_map::decode_color
//!   lk-* ...                              linked.rs (encode, parse, find, rebuild, uuids, type)
//!   est NAME RESULT                       psd_export::estimate_psd_size (copied) + version
//!   mc-exp NAME HASH LEN W B64            Multichannel document_to_psd_with: file bytes (exact)
//!   mc-imp NAME CHANNELS                  psd_to_document of that file: the channels
//!   imp NAME LOSSY B64 RESULT             io::import of a file made by the reference's encoders
//!   d2i DOC RESULT                        io::document_to_image (image summary and warnings)
//!   exp DOC EXT RESULT                    io::export, then the bytes decoded (pcraft: reloaded)
//!   raw NAME B64 RESULT | rawt ... | cam  raw.rs
//!   tl-* ...                              tiff_layers.rs
//!   mrg NAME B64 RESULT | mrgc PATH RES   io::merged_composite of synthetic / corpus PSDs
//!   corp PATH RESULT                      io::import of corpus files (pngsuite, exr)
//!   err CASE RESULT | det CASE BOOL       dispatch errors, detection helpers
//!
//! RESULT of an import: `ok DOC W[warnings] ro pv` or `err KIND "message"`; DOC lists size, mode,
//! depth, dpi bits, profile / EXIF / XMP hashes, every layer (name, flags, blend, opacity bits,
//! kind, surface: format, content bounds, default pixel, BLAKE3 of the bytes) and channel.
use std::sync::Arc;

use base64::Engine;
use photocraft_codecs::{self as codecs, ChannelLayout as L, EncodeOptions, Format, Image, Metadata, SampleType as CS};
use photocraft_color::{BlendMode, Color, ColorMode, PixelFormat, SampleType};
use photocraft_doc::{AlphaChannel, ColorIndicates, Document, Layer, LayerContent, LayerMask, LabelColor, Size};
use photocraft_geom::Rect;
use photocraft_io::{ExportOptions, IoError, ImportResult, XmpEmbed};
use photocraft_raster::Surface;

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

// ---------------------------------------------------------------------------------------------
// summaries
// ---------------------------------------------------------------------------------------------

fn surf(s: &Surface, lossy: bool) -> String {
    let fmt = s.format();
    let b = s.content_bounds();
    let mut def = vec![0u8; fmt.channels() * fmt.sample.bytes()];
    photocraft_raster::encode_pixel(&fmt, &s.default_pixel(), &mut def);
    let px = if lossy {
        "lossy".to_string()
    } else if b.is_empty() {
        h16(&[])
    } else {
        h16(&s.to_interleaved(b))
    };
    format!("{:?}/{:?}/{}:{},{},{},{}:{}:{}", fmt.mode, fmt.sample, u8::from(fmt.alpha), b.x0, b.y0, b.x1, b.y1, hexs(&def), px)
}

fn layer_sum(l: &Layer, lossy: bool) -> String {
    let k = &l.locks;
    let flags = format!("{}{}{}{}{}{}", u8::from(l.visible), u8::from(k.transparency), u8::from(k.pixels), u8::from(k.position), u8::from(k.artboard), u8::from(k.all));
    let content = match &l.content {
        LayerContent::Group(g) => format!("{{{}}}", g.children.iter().map(|c| layer_sum(c, lossy)).collect::<Vec<_>>().join(";")),
        _ => l.surface().map_or("-".into(), |s| surf(s, lossy)),
    };
    format!(
        "{:?}|{}|{:?}|{}|{}|{}|{}|{}",
        l.name,
        flags,
        l.blend,
        fbits(l.opacity),
        fbits(l.fill_opacity),
        u8::from(l.clipped),
        l.content.kind_name(),
        content
    )
}

fn color_s(c: &Color) -> String {
    format!("{:?}:{}:{}:{}:{}:{}", c.mode, fbits(c.c[0]), fbits(c.c[1]), fbits(c.c[2]), fbits(c.c[3]), fbits(c.alpha))
}

fn chan_sum(c: &AlphaChannel) -> String {
    let spot = c.spot.as_ref().map_or("-".into(), |(col, s)| format!("{}@{}", color_s(col), fbits(*s)));
    let ind = if c.indicates == ColorIndicates::SelectedAreas { "sel" } else { "mask" };
    format!("{:?}|{}|{}|{}|{}|{}", c.name, spot, color_s(&c.color), fbits(c.opacity), ind, surf(&c.surface, false))
}

fn doc_sum(d: &Document, lossy: bool) -> String {
    format!(
        "{}x{} {:?} {:?} dpi={} icc={} exif={} xmp={} L[{}] C[{}]",
        d.size.width,
        d.size.height,
        d.mode,
        d.depth,
        fbits(d.resolution_dpi),
        opt_h(d.icc_profile.as_deref().map(|v| v.as_slice())),
        opt_h(d.metadata.exif.as_deref().map(|v| v.as_slice())),
        opt_h(d.metadata.xmp.as_deref().map(str::as_bytes)),
        d.layers.iter().map(|l| layer_sum(l, lossy)).collect::<Vec<_>>().join(";"),
        d.channels.iter().map(chan_sum).collect::<Vec<_>>().join(";")
    )
}

fn warn_s(w: &[String]) -> String {
    format!("W{w:?}")
}

fn err_kind(e: &IoError) -> &'static str {
    match e {
        IoError::Psd(_) => "psd",
        IoError::Codec(_) => "codec",
        IoError::UnknownFormat(_) => "unknown",
        IoError::Unsupported(_) => "unsupported",
        IoError::Pcraft(_) => "pcraft",
        IoError::Raw(_) => "raw",
        IoError::Svg(_) => "svg",
        IoError::Cancelled => "cancelled",
        IoError::Pdn(_) => "pdn",
        IoError::Ora(_) => "ora",
        IoError::Kra(_) => "kra",
    }
}

fn err_s(e: &IoError) -> String {
    format!("err {} {:?}", err_kind(e), e.to_string())
}

fn imp_s(r: &Result<ImportResult, IoError>, lossy: bool) -> String {
    match r {
        Ok(r) => format!("ok {} {} {} {}", doc_sum(&r.document, lossy), warn_s(&r.warnings), u8::from(r.source_read_only), u8::from(r.preview_only)),
        Err(e) => err_s(e),
    }
}

fn img_s(img: &Image, lossy: bool) -> String {
    let m = &img.meta;
    let (w, h) = img.dimensions();
    format!(
        "{:?} {:?} {}x{} px={} icc={} exif={} o={} xmp={} dpi={} text={} psr={} psl={}",
        img.layout(),
        img.sample_type(),
        w,
        h,
        if lossy { "lossy".into() } else { h16(img.data()) },
        opt_h(img.icc.as_deref()),
        opt_h(m.exif.as_deref()),
        m.exif.as_deref().map_or(0, codecs::exif_orientation),
        opt_h(m.xmp.as_deref().map(str::as_bytes)),
        m.dpi.map_or("-".into(), |(x, y)| format!("{}/{}", fbits(x), fbits(y))),
        m.text.len(),
        opt_h(m.photoshop_resources.as_deref()),
        opt_h(m.photoshop_layers.as_deref())
    )
}

// ---------------------------------------------------------------------------------------------
// pixels.rs
// ---------------------------------------------------------------------------------------------

fn pixels() {
    use photocraft_io::pixels::*;
    for n in [1usize, 7, 64, 1000] {
        for s in [SampleType::U8, SampleType::U16, SampleType::F32] {
            let bps = s.bytes();
            let planes: Vec<Vec<u8>> = (0..4).map(|c| (0..n * bps).map(|i| (i * 13 + c * 5) as u8).collect()).collect();
            let mut refs: Vec<Option<&[u8]>> = planes.iter().map(|p| Some(&p[..])).collect();
            refs[2] = None;
            // A short plane: the missing pixels take the fill.
            let short = &planes[3][..(n * bps) / 2];
            refs[3] = Some(short);
            let fill: Vec<Vec<u8>> = (0..4).map(|c| vec![c as u8 * 40 + 1; bps]).collect();
            let invert = [false, true, false, true];
            let il = interleave(&refs, &fill, n, s, &invert);
            println!("px-il\t{n}\t{s:?}\t{}", h16(&il));
            let back = deinterleave(&il, 4, s, &invert);
            let flat: Vec<u8> = back.concat();
            println!("px-dl\t{n}\t{s:?}\t{}", h16(&flat));
        }
    }
    let mut all = Vec::new();
    for v in 0..=u16::MAX {
        for _ in 0..3 {
            all.extend_from_slice(&v.to_ne_bytes());
        }
    }
    lab16_chroma(&mut all, 3, true);
    let to_doc = h16(&all);
    lab16_chroma(&mut all, 3, false);
    println!("px-lab\t{to_doc}\t{}", h16(&all));
    let vals: [f32; 14] = [0.0, 1.0, 0.5, -0.25, 1.75, f32::NAN, -0.0, 1.0 / 255.0, 0.5 / 255.0, 0.499_999_97, 65534.5 / 65535.0, f32::INFINITY, -f32::INFINITY, 0.123_456_79];
    for s in [SampleType::U8, SampleType::U16, SampleType::F32] {
        for v in vals {
            let mut o = Vec::new();
            encode_be(v, s, &mut o);
            println!("px-be\t{s:?}\t{:08x}\t{}", v.to_bits(), hexs(&o));
        }
    }
    let mut h = Vec::new();
    for ci in 0..=20 {
        for ai in 0..=20 {
            let c = ci as f32 / 16.0 - 0.1;
            let a = ai as f32 / 20.0;
            for w in [0.0f32, 1.0] {
                h.extend_from_slice(&matte(c, a, w).to_bits().to_le_bytes());
                h.extend_from_slice(&unmatte(c, a, w).to_bits().to_le_bytes());
            }
        }
    }
    println!("px-matte\t{}", h16(&h));
}

// ---------------------------------------------------------------------------------------------
// channel_map.rs (crate-private: verbatim copies of display_info / apply_display_info /
// decode_color, so their exact output is pinned here)
// ---------------------------------------------------------------------------------------------

mod cm {
    use photocraft_color::{Color, ColorMode};
    use photocraft_doc::{AlphaChannel, ColorIndicates};
    const SPACE_RGB: u16 = 0;
    const SPACE_HSB: u16 = 1;
    const SPACE_CMYK: u16 = 2;
    const SPACE_LAB: u16 = 7;
    const SPACE_GRAY: u16 = 8;

    pub fn display_info(channels: &[&AlphaChannel], transparency: bool) -> Vec<u8> {
        let mut out = 1u32.to_be_bytes().to_vec();
        if transparency {
            out.extend_from_slice(&[0, 0, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 100, 1]);
        }
        for c in channels {
            let (color, opacity, kind) = match c.spot {
                Some((ink, solidity)) => (ink, solidity, 2u8),
                None => (c.color, c.opacity, if c.indicates == ColorIndicates::SelectedAreas { 0 } else { 1 }),
            };
            let rgb = color.to_rgb();
            out.extend_from_slice(&SPACE_RGB.to_be_bytes());
            for v in [rgb[0], rgb[1], rgb[2], 0.0] {
                out.extend_from_slice(&((v.clamp(0.0, 1.0) * 65535.0).round() as u16).to_be_bytes());
            }
            out.extend_from_slice(&((opacity.clamp(0.0, 1.0) * 100.0).round() as u16).to_be_bytes());
            out.push(kind);
        }
        out
    }

    pub fn apply_display_info(data: &[u8], versioned: bool, skip: usize, channels: &mut [AlphaChannel]) {
        let (body, size) = if versioned { (data.get(4..).unwrap_or(&[]), 13) } else { (data, 14) };
        for (c, rec) in channels.iter_mut().zip(body.chunks_exact(size).skip(skip)) {
            let u = |i: usize| u16::from_be_bytes([rec[i], rec[i + 1]]);
            let comps = [u(2), u(4), u(6), u(8)];
            let color = decode_color(u(0), comps);
            let opacity = f32::from(u(10).min(100)) / 100.0;
            match rec[12] {
                2 => c.spot = Some((color, opacity)),
                k => {
                    c.spot = None;
                    c.color = color;
                    c.opacity = opacity;
                    c.indicates = if k == 0 { ColorIndicates::SelectedAreas } else { ColorIndicates::MaskedAreas };
                }
            }
        }
    }

    fn hsb_to_rgb(h: f32, s: f32, v: f32) -> [f32; 3] {
        let h = if h.is_finite() { h.rem_euclid(360.0) / 60.0 } else { 0.0 };
        let (s, v) = (s.clamp(0.0, 1.0), v.clamp(0.0, 1.0));
        let f = h - h.floor();
        let (p, q, t) = (v * (1.0 - s), v * (1.0 - s * f), v * (1.0 - s * (1.0 - f)));
        match h.floor() as u8 {
            0 => [v, t, p],
            1 => [q, v, p],
            2 => [p, v, t],
            3 => [p, q, v],
            4 => [t, p, v],
            _ => [v, p, q],
        }
    }

    pub fn decode_color(space: u16, c: [u16; 4]) -> Color {
        let n = |v: u16| f32::from(v) / 65535.0;
        match space {
            SPACE_HSB => {
                let rgb = hsb_to_rgb(n(c[0]) * 360.0, n(c[1]), n(c[2]));
                Color::rgb(rgb[0], rgb[1], rgb[2])
            }
            SPACE_CMYK => {
                let cmyk = Color { mode: ColorMode::Cmyk, c: [1.0 - n(c[0]), 1.0 - n(c[1]), 1.0 - n(c[2]), 1.0 - n(c[3])], alpha: 1.0 };
                let rgb = cmyk.to_rgb();
                Color::rgb(rgb[0], rgb[1], rgb[2])
            }
            SPACE_LAB => {
                let l = f32::from(c[0].min(10000)) / 10000.0;
                let a = f32::from(c[1] as i16) / 100.0;
                let b = f32::from(c[2] as i16) / 100.0;
                let lab = Color { mode: ColorMode::Lab, c: [l, (a + 128.0) / 255.0, (b + 128.0) / 255.0, 0.0], alpha: 1.0 };
                let rgb = lab.to_rgb();
                Color::rgb(rgb[0], rgb[1], rgb[2])
            }
            SPACE_GRAY => {
                let v = 1.0 - f32::from(c[0].min(10000)) / 10000.0;
                Color::rgb(v, v, v)
            }
            _ => Color::rgb(n(c[0]), n(c[1]), n(c[2])),
        }
    }
}

/// The channel_map cases (mirrored in test_core_parity.c's cm_channels).
fn cm_channels() -> Vec<AlphaChannel> {
    let g = |x1: i32| {
        let mut s = Surface::new(PixelFormat::GRAY8);
        s.fill_rect(Rect::new(0, 0, x1, 2), &[1.0]);
        s
    };
    let mut a = AlphaChannel::new("A", g(1));
    a.color = Color::rgb(0.0, 1.0, 0.0);
    a.opacity = 0.3;
    a.indicates = ColorIndicates::SelectedAreas;
    let mut s = AlphaChannel::new("S", g(2));
    s.spot = Some((Color::rgb(1.0, 0.0, 1.0), 0.8));
    let mut c = AlphaChannel::new("C", g(3));
    c.color = Color { mode: ColorMode::Cmyk, c: [0.2, 0.4, 0.6, 0.1], alpha: 1.0 };
    c.opacity = 1.7;
    let mut l = AlphaChannel::new("L", g(1));
    l.spot = Some((Color { mode: ColorMode::Lab, c: [0.5, 0.7, 0.2, 0.0], alpha: 1.0 }, -0.5));
    let mut n = AlphaChannel::new("N", g(1));
    n.color = Color::rgb(f32::NAN, 2.0, -1.0);
    n.opacity = f32::NAN;
    vec![a, s, c, l, n]
}

fn channel_map() {
    let chans = cm_channels();
    let refs: Vec<&AlphaChannel> = chans.iter().collect();
    let di = cm::display_info(&refs, false);
    println!("cm-di\tall\t{}", hexs(&di));
    println!("cm-di\tnone\t{}", hexs(&cm::display_info(&[], false)));
    println!("cm-di\ttransp\t{}", hexs(&cm::display_info(&refs, true)));
    let fresh = || -> Vec<AlphaChannel> { (0..5).map(|i| AlphaChannel::new(format!("x{i}"), Surface::new(PixelFormat::GRAY8))).collect() };
    // 1007 (14-byte records): the 1077 records re-padded to 14 bytes, no version.
    let mut old = Vec::new();
    for rec in di[4..].chunks_exact(13) {
        old.extend_from_slice(rec);
        old.push(0);
    }
    old[12 + 14] = 1; // 1007 second record kind -> masked areas
    let kinds = [
        vec![0u8, 0, 0, 1],
        vec![0, 8, 0x13, 0x88, 0, 0, 0, 0, 0, 0, 0, 250, 7],
        vec![0, 7, 0x27, 0x10, 0x80, 0, 0x7f, 0xff, 0, 0, 0, 50, 2],
    ]
    .concat();
    let cases: Vec<(&str, Vec<u8>, bool, usize)> = vec![
        ("v", di.clone(), true, 0),
        ("transp", cm::display_info(&refs, true), true, 1),
        ("old", old, false, 0),
        ("short", di[..10].to_vec(), true, 0),
        ("partial", di[..4 + 13 * 2 + 5].to_vec(), true, 0),
        ("tiny", vec![0, 0], true, 0),
        ("kinds", kinds, true, 0),
    ];
    for (name, data, versioned, skip) in cases {
        let mut ch = fresh();
        cm::apply_display_info(&data, versioned, skip, &mut ch);
        let s: Vec<String> = ch.iter().map(|c| format!("{}|{}|{}|{}", c.spot.as_ref().map_or("-".into(), |(col, s)| format!("{}@{}", color_s(col), fbits(*s))), color_s(&c.color), fbits(c.opacity), u8::from(c.indicates == ColorIndicates::SelectedAreas))).collect();
        println!("cm-apply\t{name}\t{}", s.join(";"));
    }
    let comps: [[u16; 4]; 7] = [[0, 0, 0, 0], [65535, 65535, 65535, 65535], [10000, 0, 0, 0], [5000, 0x8000, 0x7fff, 3], [12345, 54321, 33333, 1], [20000, 0xff00, 0x0100, 9], [65535, 0, 32768, 65535]];
    for space in [0u16, 1, 2, 7, 8, 9, 65535] {
        for c in comps {
            let col = cm::decode_color(space, c);
            println!("cm-color\t{space}\t{}\t{}\t{}\t{}\t{}", c[0], c[1], c[2], c[3], color_s(&col));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// linked.rs
// ---------------------------------------------------------------------------------------------

fn lf_s(f: &photocraft_io::linked::LinkedFile) -> String {
    format!("{:?}|{:?}|{}", f.uuid, f.file_name, opt_h(Some(&f.bytes)))
}

fn linked() {
    use photocraft_io::linked::*;
    let files = vec![
        LinkedFile { uuid: "abc-1".into(), file_name: "a.png".into(), bytes: vec![0x89, b'P', b'N', b'G', 1, 2] },
        LinkedFile { uuid: "def-2".into(), file_name: "b \u{1F600}.psb".into(), bytes: b"8BPS\0\x02rest".to_vec() },
        LinkedFile { uuid: "x".repeat(300), file_name: String::new(), bytes: vec![] },
        LinkedFile { uuid: "j".into(), file_name: "c\0".into(), bytes: vec![0xFF, 0xD8, 9] },
        LinkedFile { uuid: "t\u{e9}".into(), file_name: "t.tif".into(), bytes: b"MM\0\x2a....".to_vec() },
        LinkedFile { uuid: "g".into(), file_name: "g.gif".into(), bytes: b"GIF89a".to_vec() },
        LinkedFile { uuid: "i".into(), file_name: "i".into(), bytes: b"II\x2a\0".to_vec() },
        LinkedFile { uuid: "p".into(), file_name: "p".into(), bytes: b"8BPS\0\x01".to_vec() },
    ];
    for (i, f) in files.iter().enumerate() {
        println!("lk-enc\t{i}\t{}", hexs(&encode_linked_file(f)));
        println!("lk-type\t{i}\t{}", hexs(&file_type(&f.bytes)));
    }
    let mut block: Vec<u8> = files.iter().flat_map(encode_linked_file).collect();
    let parsed = parse_linked_files(&block);
    println!("lk-parse\tall\t{}", parsed.iter().map(lf_s).collect::<Vec<_>>().join(";"));
    println!("lk-uuids\tall\t{:?}", block_uuids(&block));
    // Truncation sweep: every prefix.
    let mut h = Vec::new();
    for n in 0..=block.len() {
        let p = parse_linked_files(&block[..n]);
        h.extend_from_slice(format!("{}:{}|", n, p.iter().map(lf_s).collect::<Vec<_>>().join(";")).as_bytes());
        h.extend_from_slice(format!("{:?}|", block_uuids(&block[..n])).as_bytes());
    }
    println!("lk-trunc\t{}", h16(&h));
    // Byte flips.
    let mut h = Vec::new();
    for i in 0..block.len() {
        let mut b = block.clone();
        b[i] ^= 0x5a;
        let p = parse_linked_files(&b);
        h.extend_from_slice(format!("{}:{}|{:?}|", i, p.iter().map(lf_s).collect::<Vec<_>>().join(";"), block_uuids(&b)).as_bytes());
    }
    println!("lk-flip\t{}", h16(&h));
    // An item with an open descriptor and a non-liFD item.
    let mut item = b"liFD".to_vec();
    item.extend_from_slice(&7u32.to_be_bytes());
    item.push(1);
    item.push(b'q');
    item.extend_from_slice(&2u32.to_be_bytes());
    item.extend_from_slice(&[0, b'n', 0, 0]);
    item.extend_from_slice(b"PNGf8BIM");
    item.extend_from_slice(&3u64.to_be_bytes());
    item.push(1);
    let desc = {
        let d = photocraft_psd::descriptor::Descriptor::new("null").with("Nm  ", photocraft_psd::descriptor::Value::Integer(5));
        let mut v = 16u32.to_be_bytes().to_vec();
        v.extend_from_slice(&d.to_bytes());
        v
    };
    item.extend_from_slice(&desc);
    item.extend_from_slice(&[7, 8, 9]);
    let mut ext = item.clone();
    ext[..4].copy_from_slice(b"liFE");
    let mut data = Vec::new();
    for it in [&item, &ext] {
        data.extend_from_slice(&(it.len() as u64).to_be_bytes());
        data.extend_from_slice(it);
        while data.len() % 4 != 0 {
            data.push(0);
        }
    }
    println!("lk-desc\t{}\t{}\t{:?}", b64(&data), parse_linked_files(&data).iter().map(lf_s).collect::<Vec<_>>().join(";"), block_uuids(&data));
    // find_linked_file over global blocks.
    let mut meta = photocraft_doc::Metadata::default();
    meta.psd_global_blocks.push((*b"8BIM", *b"Patt", Arc::new(block.clone())));
    meta.psd_global_blocks.push((*b"8BIM", *b"lnk3", Arc::new(data.clone())));
    meta.psd_global_blocks.push((*b"8BIM", *b"lnk2", Arc::new(block.clone())));
    for u in ["abc-1", "def-2", "q", "zzz", "", "j"] {
        println!("lk-find\t{u}\t{}", find_linked_file(&meta, u).map_or("-".into(), |f| lf_s(&f)));
    }
    // rebuild_block.
    let add = vec![LinkedFile { uuid: "new".into(), file_name: "n.png".into(), bytes: vec![0x89, b'P', b'N', b'G'] }];
    let keep_all = |_: &str| true;
    let drop_def = |u: &str| u != "def-2";
    println!("lk-rebuild\tsame\t{}", rebuild_block(&block, &keep_all, &[]).map_or("-".into(), |b| hexs(&b)));
    println!("lk-rebuild\tdrop\t{}", rebuild_block(&block, &drop_def, &[]).map_or("-".into(), |b| hexs(&b)));
    println!("lk-rebuild\tadd\t{}", rebuild_block(&block, &keep_all, &add).map_or("-".into(), |b| hexs(&b)));
    block.extend_from_slice(&[0, 0, 0, 0, 0, 0, 1, 0, 9]);
    println!("lk-rebuild\ttail\t{}", rebuild_block(&block, &drop_def, &add).map_or("-".into(), |b| hexs(&b)));
    println!("lk-uuids\ttail\t{:?}", block_uuids(&block));
}

// ---------------------------------------------------------------------------------------------
// documents (mirrored in tests/io/common.h)
// ---------------------------------------------------------------------------------------------

/// tests/common::pattern: values on the 8-bit grid.
fn pattern(fmt: PixelFormat, r: Rect, seed: u32, alpha: bool) -> Surface {
    let mut s = Surface::new(fmt);
    let n = fmt.channels();
    let mut vals = Vec::new();
    for y in r.y0..r.y1 {
        for x in r.x0..r.x1 {
            for c in 0..n {
                let v = if c == n - 1 && fmt.alpha {
                    if alpha { (i64::from(x + y) * 37 + i64::from(seed)).rem_euclid(256) } else { 255 }
                } else {
                    (i64::from(x * 13 + y * 7) + i64::from(seed) * 31 + c as i64 * 50).rem_euclid(256)
                };
                vals.push(v as f32 / 255.0);
            }
        }
    }
    s.write_region(r, &vals);
    s.prune();
    s
}

fn raster(name: &str, fmt: PixelFormat, r: Rect, seed: u32, alpha: bool) -> Layer {
    Layer::new(name, LayerContent::Raster(pattern(fmt, r, seed, alpha)))
}

fn mask_surface(sample: SampleType, r: Rect, default: f32, seed: u32) -> Surface {
    let fmt = PixelFormat::new(ColorMode::Grayscale, sample, false);
    let mut s = Surface::with_default(fmt, &[default]);
    let vals: Vec<f32> = (0..r.width() * r.height()).map(|i| ((i * 29 + seed) % 256) as f32 / 255.0).collect();
    s.write_region(r, &vals);
    s.prune();
    s
}

fn g(v: u8) -> f32 {
    f32::from(v) / 255.0
}

/// tests/common::gen_doc with Features::PIXELS.
fn gen_doc(mode: ColorMode, depth: SampleType) -> Document {
    let mut d = Document::new("gen", Size::new(24, 16), mode, depth);
    let fmt = d.pixel_format();
    let mut bg = raster("Background", fmt, d.bounds(), 1, false);
    bg.locks.transparency = true;
    bg.locks.position = true;
    d.layers.push(bg);
    let mut neg = raster("Negative \u{1F600}", fmt, Rect::new(-5, -3, 7, 6), 2, true);
    neg.opacity = g(200);
    neg.fill_opacity = g(128);
    neg.blend = BlendMode::Multiply;
    neg.label = LabelColor::Blue;
    neg.advanced.blend_interior = true;
    neg.advanced.transparency_shapes = false;
    d.layers.push(neg);
    let mut hidden = raster("Hidden", fmt, Rect::new(3, 3, 9, 9), 3, true);
    hidden.visible = false;
    d.layers.push(hidden);
    let mut clipped = raster("Clipped", fmt, Rect::new(0, 0, 10, 10), 4, true);
    clipped.clipped = true;
    clipped.blend = BlendMode::Screen;
    d.layers.push(clipped);
    d.layers.push(Layer::new("Empty", LayerContent::Raster(Surface::new(fmt))));
    let mut m = raster("Masked", fmt, Rect::new(2, 2, 14, 12), 5, true);
    m.mask = Some(LayerMask { surface: mask_surface(depth, Rect::new(4, 4, 10, 9), 1.0, 1), enabled: true, linked: true, density: 1.0, feather: 0.0 });
    m.advanced.layer_mask_hides_effects = true;
    m.advanced.vector_mask_hides_effects = true;
    d.layers.push(m);
    let mut m2 = raster("Masked2", fmt, Rect::new(-2, 1, 6, 5), 6, true);
    m2.mask = Some(LayerMask { surface: mask_surface(depth, Rect::new(-1, 0, 3, 4), 0.0, 2), enabled: false, linked: false, density: g(128), feather: 2.5 });
    d.layers.push(m2);
    let inner_child = raster("Inner child", fmt, Rect::new(1, 1, 5, 5), 7, true);
    let mut inner = Layer::group("Inner", vec![inner_child]);
    inner.blend = BlendMode::Multiply;
    inner.opacity = g(180);
    if let LayerContent::Group(gr) = &mut inner.content {
        gr.expanded = false;
    }
    inner.advanced.knockout = photocraft_doc::Knockout::Shallow;
    let a = raster("Outer child", fmt, Rect::new(6, 2, 12, 8), 8, true);
    let mut outer = Layer::group("Outer", vec![a, inner]);
    outer.locks.position = true;
    outer.label = LabelColor::Red;
    outer.advanced.knockout = photocraft_doc::Knockout::Deep;
    outer.advanced.blend_clipped = false;
    d.layers.push(outer);
    d.layers.push(Layer::group("Empty group", vec![]));
    for (i, m) in BlendMode::LAYER_MODES.iter().enumerate() {
        if *m == BlendMode::Dissolve {
            continue;
        }
        let mut l = raster(&format!("blend {}", m.label()), fmt, Rect::from_xywh(i as i32 % 20, i as i32 % 12, 3, 2), 10 + i as u32, true);
        l.blend = *m;
        d.layers.push(l);
    }
    d
}

/// A 9x6 one-layer document (tests/flat.rs single).
fn single(mode: ColorMode, depth: SampleType, alpha: bool) -> Document {
    let mut d = Document::new("s", Size::new(9, 6), mode, depth);
    let fmt = d.pixel_format();
    d.layers.push(raster("Background", fmt, d.bounds(), 3, alpha));
    d
}

/// 16-pixel-wide columns of straight colours (tests/flat.rs columns).
fn columns(mode: ColorMode, depth: SampleType, cols: &[&[f32]]) -> Document {
    let mut d = Document::new("c", Size::new(16 * cols.len() as u32, 16), mode, depth);
    let mut s = Surface::new(d.pixel_format());
    for (i, px) in cols.iter().enumerate() {
        let x = 16 * i as i32;
        s.fill_rect(Rect::new(x, 0, x + 16, 16), px);
    }
    d.layers.push(Layer::new("Layer", LayerContent::Raster(s)));
    d
}

fn exif6() -> Vec<u8> {
    let mut v = b"II*\0\x08\0\0\0\x01\0\x12\x01\x03\0\x01\0\0\0".to_vec();
    v.extend_from_slice(&6u16.to_le_bytes());
    v.extend_from_slice(&[0, 0, 0, 0, 0, 0]);
    v
}

/// Every export document, by name (the C test builds the same list).
fn export_docs() -> Vec<(String, Document)> {
    use photocraft_cms::Builtin;
    let mut v = Vec::new();
    for mode in [ColorMode::Rgb, ColorMode::Grayscale, ColorMode::Cmyk, ColorMode::Lab] {
        for depth in [SampleType::U8, SampleType::U16, SampleType::F32] {
            for alpha in [false, true] {
                v.push((format!("single-{mode:?}-{depth:?}-{}", u8::from(alpha)), single(mode, depth, alpha)));
            }
        }
    }
    for (mode, depth) in [(ColorMode::Rgb, SampleType::U8), (ColorMode::Rgb, SampleType::U16), (ColorMode::Rgb, SampleType::F32), (ColorMode::Grayscale, SampleType::U8), (ColorMode::Cmyk, SampleType::U8), (ColorMode::Lab, SampleType::U16)] {
        v.push((format!("gen-{mode:?}-{depth:?}"), gen_doc(mode, depth)));
    }
    let rgb: [&[f32]; 3] = [&[0.0, 0.0, 0.0, 0.0], &[0.0, 0.0, 1.0, 0.5], &[0.0, 0.63, 0.0, 1.0]];
    let gray: [&[f32]; 2] = [&[0.0, 0.0], &[0.0, 0.5]];
    let cmyk: [&[f32]; 2] = [&[1.0, 1.0, 1.0, 1.0, 0.0], &[0.0, 0.0, 0.0, 1.0, 0.5]];
    v.push(("cols-rgb8".into(), columns(ColorMode::Rgb, SampleType::U8, &rgb)));
    v.push(("cols-rgb16".into(), columns(ColorMode::Rgb, SampleType::U16, &rgb)));
    v.push(("cols-rgbf".into(), columns(ColorMode::Rgb, SampleType::F32, &rgb)));
    v.push(("cols-gray".into(), columns(ColorMode::Grayscale, SampleType::U8, &gray)));
    v.push(("cols-cmyk".into(), columns(ColorMode::Cmyk, SampleType::U8, &cmyk)));
    let tagged: [&[f32]; 3] = [&[0.0, 0.05, 0.0, 1.0], &[0.2, 0.5, 0.8, 1.0], &[1.0, 0.25, 0.0, 1.0]];
    for (b, depth) in [(Builtin::LinearSrgb, SampleType::F32), (Builtin::LinearSrgb, SampleType::U16), (Builtin::DisplayP3, SampleType::U8), (Builtin::Srgb, SampleType::U8)] {
        let mut d = columns(ColorMode::Rgb, depth, &tagged);
        d.icc_profile = Some(b.profile().to_bytes());
        v.push((format!("icc-{b:?}-{depth:?}"), d));
    }
    let mut d = single(ColorMode::Cmyk, SampleType::U8, false);
    d.icc_profile = Some(Builtin::CoatedCmyk.profile().to_bytes());
    v.push(("icc-cmyk".into(), d));
    let mut d = single(ColorMode::Grayscale, SampleType::U16, false);
    d.icc_profile = Some(Builtin::GrayGamma22.profile().to_bytes());
    v.push(("icc-gray".into(), d));
    let mut d = single(ColorMode::Rgb, SampleType::U8, false);
    d.resolution_dpi = 300.0;
    d.metadata.xmp = Some(r#"<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:Description tiff:Orientation="6"/></x:xmpmeta>"#.into());
    d.metadata.exif = Some(Arc::new(exif6()));
    v.push(("meta".into(), d));
    let mut d = Document::with_background("m", Size::new(4, 2), ColorMode::Rgb, SampleType::U8, Color::rgba(1.0, 0.0, 0.0, 1.0));
    d.mode = ColorMode::Indexed;
    d.color_table = Some(photocraft_doc::ColorTable { colors: vec![[0, 0, 0], [255, 0, 0]], transparent: None });
    v.push(("indexed".into(), d));
    let mut d = gen_doc(ColorMode::Rgb, SampleType::U8);
    d.mode = ColorMode::Indexed;
    d.color_table = Some(photocraft_doc::ColorTable { colors: vec![[0, 0, 0], [255, 255, 255], [200, 30, 30], [30, 200, 30]], transparent: Some(1) });
    v.push(("indexed-t".into(), d));
    let mut d = Document::with_background("m", Size::new(4, 2), ColorMode::Grayscale, SampleType::U8, Color::rgba(0.5, 0.5, 0.5, 1.0));
    d.mode = ColorMode::Duotone;
    d.duotone = Some(photocraft_doc::Duotone { inks: vec![photocraft_doc::DuotoneInk::new("Black", [0.0; 3]), photocraft_doc::DuotoneInk::new("Orange", [1.0, 0.5, 0.0])], psd_raw: None });
    v.push(("duotone".into(), d));
    // A native single layer that isn't plain (opacity), and an empty document.
    let mut d = single(ColorMode::Rgb, SampleType::U16, true);
    d.layers[0].opacity = 0.5;
    v.push(("half".into(), d));
    v.push(("empty".into(), Document::new("e", Size::new(5, 3), ColorMode::Rgb, SampleType::U8)));
    v
}

fn multichannel_docs() -> Vec<(String, Document)> {
    let mut v = Vec::new();
    for depth in [SampleType::U8, SampleType::U16, SampleType::F32] {
        let mut d = Document::new("mc", Size::new(7, 5), ColorMode::Multichannel, depth);
        let fmt = PixelFormat::new(ColorMode::Grayscale, depth, false);
        let mut a = AlphaChannel::new("Cyan", pattern(fmt, Rect::new(0, 0, 7, 5), 4, false));
        a.spot = Some((Color::rgb(0.0, 0.6, 1.0), 0.9));
        let mut b = AlphaChannel::new("", pattern(fmt, Rect::new(1, 1, 5, 4), 9, false));
        b.spot = Some((Color::BLACK, 0.0));
        let c = AlphaChannel::new("Tint \u{e9}\u{1F600}", Surface::new(fmt));
        d.channels = vec![a, b, c];
        d.resolution_dpi = 150.0;
        d.metadata.xmp = Some("<x/>".into());
        v.push((format!("mc-{depth:?}"), d));
    }
    let mut d = Document::new("mc0", Size::new(3, 2), ColorMode::Multichannel, SampleType::U8);
    d.layers.push(Layer::raster("stray", PixelFormat::RGBA8));
    v.push(("mc-empty".into(), d));
    let mut d = Document::new("mc60", Size::new(2, 2), ColorMode::Multichannel, SampleType::U8);
    d.channels = (0..60).map(|i| AlphaChannel::new(format!("C{i}"), Surface::new(PixelFormat::GRAY8))).collect();
    v.push(("mc-60".into(), d));
    v
}

// ---------------------------------------------------------------------------------------------
// psd_export size estimate (crate-private: verbatim copy)
// ---------------------------------------------------------------------------------------------

mod est {
    use photocraft_color::{ColorMode, SampleType};
    use photocraft_doc::{Document, LayerContent};
    use photocraft_raster::Surface;
    const PSD_ESTIMATE_FIXED_OVERHEAD: u64 = 1024 * 1024;
    const PSD_ESTIMATE_LAYER_OVERHEAD: u64 = 64 * 1024;
    fn estimate_encoded_size(raw_bytes: u64, rows: u64, structural_bytes: u64) -> Option<u64> {
        raw_bytes.checked_add(raw_bytes.checked_add(63)?.checked_div(64)?)?.checked_add(rows.checked_mul(8)?)?.checked_add(structural_bytes)
    }
    fn add_plane_estimate(raw_bytes: &mut u64, rows: &mut u64, width: u64, height: u64, channels: u64, bytes_per_sample: u64) -> Option<()> {
        let plane_bytes = width.checked_mul(height)?.checked_mul(channels)?.checked_mul(bytes_per_sample)?;
        let plane_rows = height.checked_mul(channels)?;
        *raw_bytes = raw_bytes.checked_add(plane_bytes)?;
        *rows = rows.checked_add(plane_rows)?;
        Some(())
    }
    fn surface_dimensions(surface: &Surface) -> Option<(u64, u64)> {
        let bounds = surface.content_bounds();
        Some((u64::from(bounds.width()), u64::from(bounds.height())))
    }
    pub fn estimate_psd_size(doc: &Document) -> Option<u64> {
        let mut raw_bytes = 0;
        let mut rows = 0;
        let mut structural_bytes = PSD_ESTIMATE_FIXED_OVERHEAD;
        let width = u64::from(doc.size.width);
        let height = u64::from(doc.size.height);
        if doc.mode == ColorMode::Multichannel {
            let channels = u64::try_from(doc.channels.len().clamp(1, 56)).ok()?;
            let sample_bytes = u64::try_from(if doc.depth == SampleType::F32 { SampleType::U16.bytes() } else { doc.depth.bytes() }).ok()?;
            add_plane_estimate(&mut raw_bytes, &mut rows, width, height, channels, sample_bytes)?;
        } else {
            let format = doc.pixel_format();
            let color_channels = u64::try_from(format.mode.color_channels()).ok()?;
            let bytes_per_sample = u64::try_from(format.sample.bytes()).ok()?;
            let layer_channels = color_channels.checked_add(1)?;
            for (_, _, layer) in doc.walk() {
                structural_bytes = structural_bytes.checked_add(PSD_ESTIMATE_LAYER_OVERHEAD)?.checked_add(u64::try_from(layer.name.len()).ok()?)?;
                for (_, data) in &layer.psd_blocks {
                    structural_bytes = structural_bytes.checked_add(u64::try_from(data.len()).ok()?)?;
                }
                let surface = match &layer.content {
                    LayerContent::Fill(fill) => {
                        let mut fill_width = width;
                        let mut fill_height = height;
                        if let Some(cache) = &layer.fill_cache {
                            if cache.fill == *fill {
                                let (cache_width, cache_height) = surface_dimensions(&cache.surface)?;
                                fill_width = fill_width.max(cache_width);
                                fill_height = fill_height.max(cache_height);
                            }
                        }
                        add_plane_estimate(&mut raw_bytes, &mut rows, fill_width, fill_height, layer_channels, bytes_per_sample)?;
                        None
                    }
                    _ => layer.surface(),
                };
                if let Some(surface) = surface {
                    let (surface_width, surface_height) = surface_dimensions(surface)?;
                    add_plane_estimate(&mut raw_bytes, &mut rows, surface_width, surface_height, layer_channels, bytes_per_sample)?;
                }
                if let Some(mask) = &layer.mask {
                    let (mask_width, mask_height) = surface_dimensions(&mask.surface)?;
                    add_plane_estimate(&mut raw_bytes, &mut rows, mask_width, mask_height, 1, bytes_per_sample)?;
                }
                if let LayerContent::Smart(smart) = &layer.content {
                    if let photocraft_doc::SmartSource::Embedded { bytes, .. } = &smart.source {
                        structural_bytes = structural_bytes.checked_add(u64::try_from(bytes.len()).ok()?)?;
                    }
                }
            }
            let max_extra_channels = 56usize.saturating_sub(format.mode.color_channels() + 1);
            let mut extra_channels = doc.channels.len().min(max_extra_channels);
            if doc.quick_mask.is_some() && extra_channels < max_extra_channels {
                extra_channels += 1;
            }
            let merged_channels = color_channels.checked_add(1)?.checked_add(u64::try_from(extra_channels).ok()?)?;
            add_plane_estimate(&mut raw_bytes, &mut rows, width, height, merged_channels, bytes_per_sample)?;
            for channel in doc.channels.iter().take(extra_channels) {
                structural_bytes = structural_bytes.checked_add(u64::try_from(channel.name.len()).ok()?)?;
            }
        }
        structural_bytes = structural_bytes
            .checked_add(u64::try_from(doc.icc_profile.as_ref().map_or(0, |profile| profile.len())).ok()?)?
            .checked_add(u64::try_from(doc.metadata.xmp.as_ref().map_or(0, String::len)).ok()?)?
            .checked_add(u64::try_from(doc.metadata.exif.as_ref().map_or(0, |data| data.len())).ok()?)?;
        for (_, name, data) in &doc.metadata.psd_resources {
            structural_bytes = structural_bytes.checked_add(u64::try_from(name.len()).ok()?)?.checked_add(u64::try_from(data.len()).ok()?)?;
        }
        for (_, _, data) in &doc.metadata.psd_global_blocks {
            structural_bytes = structural_bytes.checked_add(u64::try_from(data.len()).ok()?)?;
        }
        estimate_encoded_size(raw_bytes, rows, structural_bytes)
    }
}

fn estimates() {
    let mut docs = export_docs();
    docs.extend(multichannel_docs());
    let mut d = gen_doc(ColorMode::Rgb, SampleType::U8);
    d.layers[1].psd_blocks.push((*b"abcd", Arc::new(vec![1; 77])));
    d.metadata.psd_resources.push((4000, "nm".into(), Arc::new(vec![9, 9, 9])));
    d.metadata.psd_global_blocks.push((*b"8BIM", *b"Patt", Arc::new(vec![0; 13])));
    d.channels.push(AlphaChannel::new("Alpha", mask_surface(SampleType::U8, Rect::new(0, 0, 24, 16), 0.0, 3)));
    d.quick_mask = Some(AlphaChannel::new("Quick Mask", Surface::new(PixelFormat::GRAY8)));
    docs.push(("blocks".into(), d));
    docs.push(("huge".into(), Document::new("h", Size::new(4_000_000_000, 4_000_000_000), ColorMode::Rgb, SampleType::F32)));
    docs.push(("big".into(), Document::new("b", Size::new(30_001, 2), ColorMode::Rgb, SampleType::U8)));
    docs.push(("2g".into(), Document::new("g", Size::new(30_000, 30_000), ColorMode::Cmyk, SampleType::U16)));
    for (name, d) in &docs {
        println!("est\t{name}\t{}", est::estimate_psd_size(d).map_or("-".into(), |v| v.to_string()));
    }
}

// ---------------------------------------------------------------------------------------------
// multichannel
// ---------------------------------------------------------------------------------------------

fn multichannel() {
    for (name, d) in multichannel_docs() {
        for force in [false, true] {
            let (file, w) = photocraft_io::document_to_psd_with(&d, &photocraft_io::PsdExportOptions { force_psb: force, ..Default::default() });
            let bytes = file.to_bytes().unwrap();
            println!("mc-exp\t{name}\t{}\t{}\t{}\t{}\t{}", u8::from(force), h16(&bytes), bytes.len(), warn_s(&w), b64(&bytes));
            if !force {
                let (back, w) = photocraft_io::psd_to_document(&file);
                println!("mc-imp\t{name}\t{}\t{}", back.channels.iter().map(chan_sum).collect::<Vec<_>>().join(";"), warn_s(&w));
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// flat import
// ---------------------------------------------------------------------------------------------

/// A synthetic codec image: values on a ramp, some out of range for floats.
fn synth(w: u32, h: u32, layout: L, sample: CS, seed: u32) -> Image {
    let ch = layout.channels();
    let mut v = Vec::new();
    for y in 0..h {
        for x in 0..w {
            for c in 0..ch {
                let mut f = ((x * 13 + y * 7 + seed * 31 + c as u32 * 50) % 256) as f32 / 255.0;
                if c == ch - 1 && layout.has_alpha() {
                    f = ((x + y) * 37 % 256) as f32 / 255.0;
                }
                if matches!(sample, CS::F16 | CS::F32) && (x + y) % 5 == 0 {
                    f = f * 2.5 - 0.5;
                }
                v.push(f);
            }
        }
    }
    match sample {
        CS::F32 => Image::from_f32(w, h, layout, &v).unwrap(),
        CS::F16 => Image::from_f16(w, h, layout, &v.iter().map(|f| codecs::f16::from_f32(*f)).collect::<Vec<_>>()).unwrap(),
        _ => Image::from_normalized(w, h, layout, sample, &v).unwrap(),
    }
}

fn emit_import(name: &str, bytes: &[u8], lossy: bool) {
    let r = photocraft_io::import(name, bytes);
    println!("imp\t{name}\t{}\t{}\t{}", u8::from(lossy), b64(bytes), imp_s(&r, lossy));
}

fn gif_frames(frames: usize) -> Vec<u8> {
    let mut b = b"GIF89a\x01\0\x01\0\x80\0\0\0\0\0\xFF\xFF\xFF".to_vec();
    for _ in 0..frames {
        b.extend_from_slice(b"\x2C\0\0\0\0\x01\0\x01\0\0\x02\x02\x44\x01\0");
    }
    b.push(0x3B);
    b
}

/// tests/tiff_pages.rs: one 8-bit gray page per (w, h, level, NewSubfileType, orientation).
fn tiff_pages(big: bool, pages: &[(u32, u32, u8, u32, u16)]) -> Vec<u8> {
    let mut f = Vec::new();
    f.extend_from_slice(b"II");
    if big {
        f.extend_from_slice(&43u16.to_le_bytes());
        f.extend_from_slice(&8u16.to_le_bytes());
        f.extend_from_slice(&0u16.to_le_bytes());
        f.extend_from_slice(&0u64.to_le_bytes());
    } else {
        f.extend_from_slice(&42u16.to_le_bytes());
        f.extend_from_slice(&0u32.to_le_bytes());
    }
    let mut next_field = if big { 8 } else { 4 };
    for &(w, h, level, kind, orientation) in pages {
        let pixels_at = f.len() as u64;
        f.extend(std::iter::repeat_n(level, (w * h) as usize));
        if f.len() % 2 == 1 {
            f.push(0);
        }
        let ifd_at = f.len() as u64;
        let width = if big { 8 } else { 4 };
        f[next_field..next_field + width].copy_from_slice(&ifd_at.to_le_bytes()[..width]);
        let entries: [(u16, u16, u64); 11] = [
            (254, 4, u64::from(kind)),
            (256, 4, u64::from(w)),
            (257, 4, u64::from(h)),
            (258, 3, 8),
            (259, 3, 1),
            (262, 3, 1),
            (273, 4, pixels_at),
            (274, 3, u64::from(orientation)),
            (277, 3, 1),
            (278, 4, u64::from(h)),
            (279, 4, u64::from(w * h)),
        ];
        if big {
            f.extend_from_slice(&(entries.len() as u64).to_le_bytes());
        } else {
            f.extend_from_slice(&(entries.len() as u16).to_le_bytes());
        }
        for (tag, ty, v) in entries {
            f.extend_from_slice(&tag.to_le_bytes());
            f.extend_from_slice(&ty.to_le_bytes());
            if big {
                f.extend_from_slice(&1u64.to_le_bytes());
                f.extend_from_slice(&v.to_le_bytes());
            } else {
                f.extend_from_slice(&1u32.to_le_bytes());
                f.extend_from_slice(&(v as u32).to_le_bytes());
            }
        }
        next_field = f.len();
        f.extend(std::iter::repeat_n(0, width));
    }
    f
}

fn flat_imports() {
    use photocraft_cms::Builtin;
    let opts = EncodeOptions::default();
    for f in Format::ALL {
        let caps = f.caps();
        if !caps.write || !caps.read {
            continue;
        }
        let ext = f.extensions()[0];
        for &layout in caps.layouts {
            for &sample in caps.depths {
                for (w, h) in [(7u32, 5u32), (1, 1)] {
                    let img = synth(w, h, layout, sample, 3);
                    let Ok(bytes) = codecs::encode(&img, f, &opts) else { continue };
                    emit_import(&format!("{w}x{h}-{layout:?}-{sample:?}.{ext}"), &bytes, f == Format::Jpeg);
                }
            }
        }
    }
    // Metadata: profile, EXIF, XMP, unequal resolution, text.
    for f in [Format::Png, Format::Jpeg, Format::Tiff, Format::WebP] {
        let meta = Metadata { exif: Some(exif6()), xmp: Some("<x:xmpmeta xmlns:x='adobe:ns:meta/'/>".into()), dpi: Some((300.0, 150.0)), text: vec![("Software".into(), "rustref".into())], ..Default::default() };
        let img = synth(6, 4, L::Rgb, CS::U8, 5).with_icc(Some(Builtin::DisplayP3.profile().to_bytes().to_vec())).with_meta(meta);
        let bytes = codecs::encode(&img, f, &opts).unwrap();
        emit_import(&format!("meta.{}", f.extensions()[0]), &bytes, f == Format::Jpeg);
        let meta = Metadata { dpi: Some((72.0, 72.004)), ..Default::default() };
        let bytes = codecs::encode(&synth(3, 2, L::Gray, CS::U8, 1).with_meta(meta), f, &opts).unwrap();
        emit_import(&format!("dpi.{}", f.extensions()[0]), &bytes, f == Format::Jpeg);
    }
    // TIFF orientation and pages.
    for o in 1..=8u16 {
        let mut t = tiff_pages(false, &[(6, 4, 50, 0, o)]);
        // Make the pixels asymmetric so every orientation hashes differently.
        for (i, b) in t.iter_mut().skip(8).take(24).enumerate() {
            *b = (i * 9) as u8;
        }
        emit_import(&format!("orient{o}.tif"), &t, false);
    }
    let pages = [(2, 2, 9, 1, 1), (6, 4, 50, 0, 1), (3, 5, 90, 2, 1)];
    for big in [false, true] {
        let t = tiff_pages(big, &pages);
        emit_import(&format!("pages-{}.tif", u8::from(big)), &t, false);
        for p in 0..4 {
            let r = photocraft_io::import_tiff_page("pages.tif", &t, Some(p));
            println!("tpage\t{}\t{p}\t{}", u8::from(big), imp_s(&r, false));
        }
        let r = photocraft_io::import_tiff_page("pages.tif", &tiff_pages(big, &[(6, 4, 50, 0, 6)]), None);
        println!("tpage\t{}\tturned\t{}", u8::from(big), imp_s(&r, false));
    }
    // Animations, truncated JPEG, EXR/HDR linear tagging, garbage.
    emit_import("one.gif", &gif_frames(1), false);
    emit_import("three.gif", &gif_frames(3), false);
    let jpeg = codecs::encode(&synth(32, 16, L::Rgb, CS::U8, 2), Format::Jpeg, &opts).unwrap();
    emit_import("cut.jpg", &jpeg[..jpeg.len() - 4], true);
    let hdr = synth(5, 3, L::Rgb, CS::F32, 4);
    for f in [Format::OpenExr, Format::Hdr] {
        if let Ok(b) = codecs::encode(&hdr.clone().with_icc(Some(Builtin::DisplayP3.profile().to_bytes().to_vec())), f, &opts) {
            emit_import(&format!("tagged.{}", f.extensions()[0]), &b, false);
        }
    }
    emit_import("noext", b"definitely not an image", false);
    emit_import("x.psd", b"8BPS\0\x01", false);
    emit_import("x.pcraft", b"PK\x03\x04garbage", false);
    emit_import("x.afphoto", b"\x89PNG", false);
    emit_import("x.svg", b"<svg/>", false);
    emit_import("noext", b"\xEF\xBB\xBF \n<?xml version='1.0'?><svg/>", false);
    emit_import("empty", b"", false);
}

// ---------------------------------------------------------------------------------------------
// flat export
// ---------------------------------------------------------------------------------------------

fn lossy_format(f: Format) -> bool {
    matches!(f, Format::Jpeg | Format::Gif)
}

fn flat_exports() {
    let docs = export_docs();
    let none = ExportOptions { xmp: XmpEmbed::None, ..ExportOptions::default() };
    for (name, d) in &docs {
        let mut w = Vec::new();
        match photocraft_io::document_to_image(d, &mut w) {
            Ok(img) => println!("d2i\t{name}\t{} {}", img_s(&img, false), warn_s(&w)),
            Err(e) => println!("d2i\t{name}\t{}", err_s(&e)),
        }
        for f in Format::ALL {
            if !f.caps().write {
                continue;
            }
            let ext = f.extensions()[0];
            let r = photocraft_io::export(d, ext, &ExportOptions::default());
            let s = match r {
                Ok(r) => match codecs::decode(&r.bytes) {
                    Ok(img) => format!("ok {} {}", warn_s(&r.warnings), img_s(&img, lossy_format(f))),
                    Err(e) => format!("ok {} undecodable {:?}", warn_s(&r.warnings), e.to_string()),
                },
                Err(e) => err_s(&e),
            };
            println!("exp\t{name}\t{ext}\t{s}");
        }
        let r = photocraft_io::export(d, "x.pcraft", &ExportOptions::default());
        let s = match r {
            Ok(r) => match photocraft_io::import("x.pcraft", &r.bytes) {
                Ok(back) => format!("ok {} {}", warn_s(&r.warnings), doc_sum(&back.document, false)),
                Err(e) => format!("ok reload {}", err_s(&e)),
            },
            Err(e) => err_s(&e),
        };
        println!("exp\t{name}\tpcraft\t{s}");
    }
    // Options and name forms on one document.
    let d = &docs.iter().find(|(n, _)| n == "meta").unwrap().1;
    for ext in ["png", "jpg", "webp", "tif"] {
        let r = photocraft_io::export(d, &format!("out.{ext}"), &none).unwrap();
        let img = codecs::decode(&r.bytes).unwrap();
        println!("expo\tnone\t{ext}\t{} {}", warn_s(&r.warnings), img_s(&img, ext == "jpg"));
    }
    let mut o = ExportOptions::default();
    o.encode.png_interlaced = true;
    o.encode.tiff_compression = codecs::TiffCompression::Lzw;
    o.encode.embed_icc = false;
    o.encode.embed_metadata = false;
    for ext in ["png", "tif"] {
        let r = photocraft_io::export(d, ext, &o).unwrap();
        let img = codecs::decode(&r.bytes).unwrap();
        println!("expo\tnometa\t{ext}\t{} {}", warn_s(&r.warnings), img_s(&img, false));
    }
    let s = &docs.iter().find(|(n, _)| n == "single-Rgb-U8-0").unwrap().1;
    for name in ["png", ".png", "a/b/c.PNG", "C:\\x\\y.png", "a.xyz", "", "noext/", "x.afphoto", "y.AF", "z.heic", "z.avif", "x.psd", "x.psb", "x.tif.gz"] {
        let r = photocraft_io::export(s, name, &ExportOptions::default());
        let res = match r {
            Ok(r) => format!("ok {} {}", warn_s(&r.warnings), codecs::detect(&r.bytes).map_or("?".into(), |f| format!("{f:?}"))),
            Err(e) => err_s(&e),
        };
        println!("err\texport:{name}\t{res}");
    }
}

// ---------------------------------------------------------------------------------------------
// raw.rs
// ---------------------------------------------------------------------------------------------

mod cam {
    pub fn camera_name(make: Option<&str>, model: Option<&str>) -> String {
        match (make, model) {
            (Some(make), Some(model))
                if make.split_whitespace().next().is_some_and(|brand| model.to_ascii_lowercase().starts_with(&brand.to_ascii_lowercase())) =>
            {
                model.to_string()
            }
            (make, model) => [make, model].into_iter().flatten().collect::<Vec<_>>().join(" "),
        }
    }
}

fn nef_with_preview(orientation: Option<u16>, preview: Vec<u8>) -> Vec<u8> {
    use photocraft_raw::testgen::{TiffBuilder, Val};
    let mut t = TiffBuilder::default();
    let strip = t.blob(vec![0; 64]);
    let len = preview.len() as u32;
    let preview = t.blob(preview);
    let raw = t.ifd(vec![
        (256, Val::Long(vec![8])),
        (257, Val::Long(vec![8])),
        (258, Val::Short(vec![12])),
        (259, Val::Short(vec![34713])),
        (262, Val::Short(vec![32803])),
        (273, Val::Blobs(vec![strip])),
        (279, Val::Long(vec![64])),
        (33421, Val::Short(vec![2, 2])),
        (33422, Val::Byte(vec![0, 1, 1, 2])),
    ]);
    let mut e = vec![(271, Val::Ascii("NIKON CORPORATION".into()))];
    if let Some(o) = orientation {
        e.push((274, Val::Short(vec![o])));
    }
    e.push((330, Val::Ifds(vec![raw])));
    e.push((513, Val::Blobs(vec![preview])));
    e.push((514, Val::Long(vec![len])));
    let ifd0 = t.ifd(e);
    t.chain = vec![ifd0];
    t.build()
}

fn raws() {
    use photocraft_io::raw::{RawTuning, import_raw, import_raw_tuned, import_raw_with};
    use photocraft_raw::testgen::{Cr2Spec, DngSpec, mosaic, scene};
    let mut files: Vec<(String, Vec<u8>)> = Vec::new();
    let (w, h) = (40, 24);
    let mut spec = DngSpec::cfa(w, h, mosaic(&scene(w, h), w, [0, 1, 1, 2], 0, 65535));
    spec.as_shot_neutral = Some([0.6, 1.0, 0.8]);
    files.push(("shot.dng".into(), spec.build()));
    let spec = DngSpec::cfa(16, 12, mosaic(&scene(16, 12), 16, [1, 0, 2, 1], 0, 65535));
    files.push(("nowb.dng".into(), spec.build()));
    let (w, h) = (48, 16);
    let data = mosaic(&scene(w, h), w, [0, 1, 1, 2], 256, 12000);
    let spec = Cr2Spec { width: w, height: h, data, precision: 14, components: 2, slices: vec![24, 24], borders: None, wb_rggb: None, orientation: 6, model_id: None };
    files.push(("IMG_0001.CR2".into(), spec.build()));
    let data = mosaic(&scene(32, 16), 32, [0, 1, 1, 2], 256, 12000);
    let spec = Cr2Spec { width: 32, height: 16, data, precision: 14, components: 2, slices: vec![16, 16], borders: None, wb_rggb: Some([2048, 1024, 1024, 1536]), orientation: 1, model_id: None };
    files.push(("wb.CR2".into(), spec.build()));
    let img = Image::from_u8(32, 20, L::Rgb, (0..32 * 20 * 3).map(|i| (i * 7 % 251) as u8).collect()).unwrap();
    let jpeg = codecs::encode(&img, Format::Jpeg, &EncodeOptions { jpeg_quality: 100, jpeg_chroma_subsampling: false, ..Default::default() }).unwrap();
    files.push(("DSC_0001.NEF".into(), nef_with_preview(None, jpeg.clone())));
    files.push(("DSC_0006.NEF".into(), nef_with_preview(Some(6), jpeg.clone())));
    let mut cr3 = vec![0, 0, 0, 24];
    cr3.extend_from_slice(b"ftypcrx ");
    cr3.extend_from_slice(&[0; 12]);
    files.push(("IMG_0001.CR3".into(), cr3));
    for (name, bytes) in &files {
        let lossy = name.ends_with("NEF");
        println!("raw\t{name}\t{}\t{}", b64(bytes), imp_s(&import_raw(name, bytes), lossy));
        let r = photocraft_io::import(name, bytes);
        println!("rawi\t{name}\t{}", imp_s(&r, lossy));
        for (t, ti, e) in [(0.0f32, 0.0f32, 0.0f32), (25.0, -10.0, 0.5), (150.0, -150.0, -1.25), (0.0, 0.0, 10.5), (0.0, 0.0, f32::NAN), (-0.4, 0.4, -0.004)] {
            let r = import_raw_tuned(name, bytes, &RawTuning { temperature: t, tint: ti, exposure: e, ..Default::default() });
            let s = match r {
                Ok((r, applied)) => format!("{} {}", u8::from(applied), imp_s(&Ok(r), false)),
                Err(e) => err_s(&e),
            };
            println!("rawt\t{name}\t{}\t{}\t{}\t{s}", fbits(t), fbits(ti), fbits(e));
        }
        for dm in [photocraft_raw::Demosaic::Bilinear, photocraft_raw::Demosaic::Mhc] {
            let o = photocraft_raw::DevelopOptions { demosaic: dm, limits: photocraft_raw::Limits::default(), ..Default::default() };
            println!("rawd\t{name}\t{}\t{}", dm.id(), imp_s(&import_raw_with(name, bytes, &o), lossy));
        }
    }
    for (make, model) in [
        (Some("Canon"), Some("Canon EOS 80D")),
        (Some("NIKON CORPORATION"), Some("NIKON D3200")),
        (Some("SONY"), Some("ILCE-7M3")),
        (None, Some("X")),
        (Some("Panasonic"), Some("DC-G9")),
        (Some("Y"), None),
        (None, None),
        (Some(""), Some("Z")),
        (Some("  \u{2003}leica camera"), Some("LEICA M10")),
        (Some("canon"), Some("Can")),
        (Some("\u{e9}t\u{e9}"), Some("\u{c9}T\u{c9} 1")),
    ] {
        println!("cam\t{}\t{}\t{:?}", make.map_or("-".into(), |m| format!("{m:?}")), model.map_or("-".into(), |m| format!("{m:?}")), cam::camera_name(make, model));
    }
}

// ---------------------------------------------------------------------------------------------
// tiff_layers.rs
// ---------------------------------------------------------------------------------------------

fn tiff_layers() {
    use photocraft_io::tiff_layers::*;
    for (name, d) in export_docs() {
        println!("tl-would\t{name}\t{}", u8::from(would_write_layers(&d)));
    }
    let layered = ExportOptions { tiff_layers: true, ..ExportOptions::default() };
    let mut docs = vec![
        ("gen8".to_string(), gen_doc(ColorMode::Rgb, SampleType::U8)),
        ("gen16".to_string(), gen_doc(ColorMode::Rgb, SampleType::U16)),
        ("genf".to_string(), gen_doc(ColorMode::Rgb, SampleType::F32)),
        ("gray".to_string(), gen_doc(ColorMode::Grayscale, SampleType::U8)),
        ("cmyk".to_string(), gen_doc(ColorMode::Cmyk, SampleType::U16)),
        ("lab".to_string(), gen_doc(ColorMode::Lab, SampleType::U8)),
        ("single".to_string(), single(ColorMode::Rgb, SampleType::U8, false)),
        ("mc".to_string(), multichannel_docs().remove(0).1),
    ];
    let mut t = Document::new("t", Size::new(4, 4), ColorMode::Rgb, SampleType::U8);
    let mut s = Surface::new(t.pixel_format());
    let px: Vec<f32> = (0..16).flat_map(|_| [1.0, 0.0, 0.0, 0.5]).collect();
    s.write_region(Rect::new(0, 0, 4, 4), &px);
    t.layers.push(Layer::new("Red", LayerContent::Raster(s)));
    docs.push(("translucent".to_string(), t));
    for (name, d) in &docs {
        for (oname, o) in [("layers", &layered), ("flat", &ExportOptions::default()), ("psb", &ExportOptions { force_psb: true, ..layered.clone() })] {
            match photocraft_io::export(d, "x.tif", o) {
                Ok(r) => {
                    let img = codecs::decode(&r.bytes).unwrap();
                    println!("tl-exp\t{name}\t{oname}\t{} {}\t{}", warn_s(&r.warnings), img_s(&img, false), b64(&r.bytes));
                    let back = photocraft_io::import("x.tif", &r.bytes);
                    println!("tl-imp\t{name}\t{oname}\t{}", imp_s(&back, false));
                    if let (true, Some(layers)) = (oname == "layers", img.meta.photoshop_layers.as_ref()) {
                        // Damaged layer data opens flattened; truncated data too.
                        let at = r.bytes.windows(layers.len()).position(|w| w == &layers[..]).unwrap();
                        let mut bad = r.bytes.clone();
                        for b in &mut bad[at + 36..at + 36 + 24] {
                            *b ^= 0xa5;
                        }
                        println!("tl-bad\t{name}\t{}\t{}", b64(&bad), imp_s(&photocraft_io::import("x.tif", &bad), false));
                    }
                }
                Err(e) => println!("tl-exp\t{name}\t{oname}\t{}\t-", err_s(&e)),
            }
        }
    }
    // strip_foreign_order_blocks on a hand-made file.
    let mut f = photocraft_psd::testgen::small(photocraft_psd::Version::Psd, photocraft_psd::Compression::Raw);
    let mut b = photocraft_psd::TaggedBlock::new(*b"zzzz", vec![1, 2, 3, 4]);
    b.signature = *b"MIB8";
    f.global_blocks.push(b.clone());
    f.global_blocks.push(photocraft_psd::TaggedBlock::new(*b"keep", vec![5, 6]));
    if let Some(info) = &mut f.layer_info {
        for l in &mut info.layers {
            l.blocks.push(b.clone());
            let mut b2 = photocraft_psd::TaggedBlock::new(*b"\xffqq\x01", vec![7; 6]);
            b2.signature = *b"46B8";
            l.blocks.push(b2);
        }
    }
    let before = f.to_bytes().unwrap();
    let w = strip_foreign_order_blocks(&mut f);
    let after = f.to_bytes().unwrap();
    println!("tl-strip\t{}\t{}\t{}", b64(&before), h16(&after), warn_s(&w));
}

// ---------------------------------------------------------------------------------------------
// merged_composite
// ---------------------------------------------------------------------------------------------

fn px_hash(px: &[[f32; 4]]) -> String {
    let mut b = Vec::with_capacity(px.len() * 16);
    for p in px {
        for v in p {
            let v = if v.is_nan() { f32::NAN } else { *v };
            b.extend_from_slice(&(if v.is_nan() { 0x7fc0_0000u32 } else { v.to_bits() }).to_le_bytes());
        }
    }
    format!("{}:{}", px.len(), h16(&b))
}

fn merged() {
    use photocraft_psd::{Compression, Version, testgen};
    let mut files = vec![("small".to_string(), testgen::small(Version::Psd, Compression::Raw))];
    for (name, d) in [("gen8", gen_doc(ColorMode::Rgb, SampleType::U8)), ("gen16", gen_doc(ColorMode::Rgb, SampleType::U16)), ("gray", gen_doc(ColorMode::Grayscale, SampleType::U8)), ("genf", gen_doc(ColorMode::Rgb, SampleType::F32)), ("cmyk", gen_doc(ColorMode::Cmyk, SampleType::U8)), ("lab", gen_doc(ColorMode::Lab, SampleType::U16))] {
        files.push((name.to_string(), photocraft_io::document_to_psd(&d)));
    }
    files.push(("mc".into(), photocraft_io::document_to_psd(&multichannel_docs().remove(0).1)));
    for (name, f) in files {
        let bytes = f.to_bytes().unwrap();
        let r = photocraft_io::merged_composite(&f);
        println!("mrg\t{name}\t{}\t{}", b64(&bytes), r.map_or_else(|e| err_s(&e), |p| px_hash(&p)));
    }
    let root = std::path::Path::new("corpus");
    for dir in ["psd", "psd-tools", "photoshop"] {
        for p in files_in(&root.join(dir), &["psd", "psb"]) {
            let Ok(bytes) = std::fs::read(&p) else { continue };
            let res = match photocraft_psd::PsdFile::from_bytes(&bytes) {
                Ok(f) => photocraft_io::merged_composite(&f).map_or_else(|e| err_s(&e), |px| px_hash(&px)),
                Err(e) => format!("parse {:?}", e.to_string()),
            };
            println!("mrgc\t{}\t{res}", p.display());
        }
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
            } else if p.extension().and_then(|e| e.to_str()).is_some_and(|e| exts.iter().any(|x| e.eq_ignore_ascii_case(x))) {
                out.push(p);
            }
        }
    }
    out.sort();
    out
}

fn corpus() {
    let root = std::path::Path::new("corpus");
    // HEIF is left out: rustref builds photocraft-io without its `heif` feature (the HEIF decode
    // goldens are tests/golden/decode/heif).
    for (dir, exts) in [("pngsuite", &["png"][..]), ("exr", &["exr"][..])] {
        for p in files_in(&root.join(dir), exts) {
            let Ok(bytes) = std::fs::read(&p) else { continue };
            let name = p.file_name().and_then(|n| n.to_str()).unwrap_or("x").to_string();
            let r = photocraft_io::import(&name, &bytes);
            println!("corp\t{}\t{}", p.display(), imp_s(&r, false));
            // Saved back as PNG (TIFF for what PNG can't hold) and reopened.
            if let Ok(r) = &r {
                let ext = if r.document.depth == SampleType::F32 || r.document.mode == ColorMode::Cmyk { "tif" } else { "png" };
                let s = match photocraft_io::export(&r.document, ext, &ExportOptions::default()) {
                    Ok(e) => imp_s(&photocraft_io::import(&format!("x.{ext}"), &e.bytes), false),
                    Err(e) => err_s(&e),
                };
                println!("corpx\t{}\t{ext}\t{s}", p.display());
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// detection
// ---------------------------------------------------------------------------------------------

fn detection() {
    let svgs: [&[u8]; 12] = [
        b"<svg/>",
        b"  \n\t<svg>",
        b"\xEF\xBB\xBF<svg",
        b"\xEF\xBB\xBF\xEF\xBB\xBF \xE2\x80\x83<svg",
        b"<?xml version='1.0'?>\n<svg/>",
        b"<?xml version='1.0'?><html/>",
        b"<!DOCTYPE svg><svg/>",
        b"<!-- c --><svg/>",
        b"<html><svg/></html>",
        b"\xFF<svg/>",
        b"",
        b"\xC2\xA0<svg",
    ];
    for (i, s) in svgs.iter().enumerate() {
        println!("det\tsvg{i}\t{}\t{}", b64(s), u8::from(photocraft_io::svg::is_svg(s)));
    }
    let mut long = vec![b' '; 4090];
    long.extend_from_slice(b"<?xml <svg");
    println!("det\tsvglong\t{}\t{}", b64(&long), u8::from(photocraft_io::svg::is_svg(&long)));
    for n in ["a.af", "A.AFPHOTO", ".afdesign", "x/y.afpub", "x.af/y", "C:\\a\\b.afphoto", "noext", "a.afx", "a.af.png"] {
        println!("det\taf:{n}\t-\t{}", u8::from(photocraft_io::affinity::has_extension(n)));
    }
    for b in [&b"8BPS"[..], b"8BP", b"8BPSx", b""] {
        println!("det\tpsd\t{}\t{}", b64(b), u8::from(photocraft_io::is_psd(b)));
    }
}

pub fn run() {
    // Debugging aid: PC_IO_CORE_PROBE=DOC:EXT prints the decoded samples of that export.
    if let Ok(p) = std::env::var("PC_IO_CORE_PROBE") {
        let (doc, ext) = p.split_once(':').unwrap_or((&p, "png"));
        let d = export_docs().into_iter().find(|(n, _)| n == doc).map(|(_, d)| d).expect("document");
        let r = photocraft_io::export(&d, ext, &ExportOptions::default()).unwrap();
        let img = codecs::decode(&r.bytes).unwrap();
        println!("{:?} {:?} {:?}", img.layout(), img.sample_type(), img.data());
        return;
    }
    pixels();
    channel_map();
    linked();
    estimates();
    multichannel();
    flat_imports();
    flat_exports();
    raws();
    tiff_layers();
    merged();
    corpus();
    detection();
}
