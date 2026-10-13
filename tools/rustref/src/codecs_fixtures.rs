//! R2: synthetic multi-part and Cryptomatte OpenEXR files written by the `exr` crate, exactly as
//! crates/codecs/tests/{exr_multipart,exr_cryptomatte}.rs build them, for tests/codecs/test_exr_parts.c.
//! Run: rustref codecs-fixtures tests/fixtures/codecs

use exr::meta::attribute::{AttributeValue, Text};
use exr::prelude::{
    AnyChannel, AnyChannels, Blocks, Compression, Encoding, FlatSamples, Image, ImageAttributes, IntegerBounds, Layer, LayerAttributes, LineOrder,
    WritableImage,
};
use photocraft_codecs::{cryptomatte_id, cryptomatte_key, f16};

fn enc(compression: Compression) -> Encoding {
    Encoding { compression, blocks: Blocks::ScanLines, line_order: LineOrder::Increasing }
}

fn layer_f32(name: &str, comp: Compression, chans: &[(&str, Vec<f32>)]) -> Layer<AnyChannels<FlatSamples>> {
    let n = chans.iter().find(|(_, v)| !v.is_empty()).map_or(0, |(_, v)| v.len());
    let list = chans.iter().map(|(cn, v)| AnyChannel::new(*cn, FlatSamples::F32(v.clone()))).collect::<Vec<_>>();
    let h = (n as f32).sqrt().round() as usize;
    Layer::new((h, h), LayerAttributes::named(name), enc(comp), AnyChannels::sort(list.into_iter().collect()))
}

fn write_image(layers: Vec<Layer<AnyChannels<FlatSamples>>>) -> Vec<u8> {
    let size: (usize, usize) = layers.first().map_or((1, 1), |l| l.size.into());
    let image = Image::from_layers(ImageAttributes::new(IntegerBounds::new((0, 0), size)), layers);
    let mut out = std::io::Cursor::new(Vec::new());
    image.write().to_buffered(&mut out).expect("synthetic exr writes");
    out.into_inner()
}

fn aov_file() -> Vec<u8> {
    let n = 16;
    let z: Vec<f32> = (0..n).map(|i| i as f32).collect();
    let beauty: [Vec<f32>; 4] = std::array::from_fn(|c| {
        (0..n).map(|i| match c { 0 => i as f32 / 15.0, 1 => 1.0 - i as f32 / 15.0, 2 => 0.25, _ => 1.0 }).collect()
    });
    let diffuse: [Vec<f32>; 3] = std::array::from_fn(|c| (0..n).map(|_| [0.3, 0.6, 0.9][c]).collect());
    write_image(vec![
        layer_f32("Z", Compression::Uncompressed, &[("Z", z)]),
        layer_f32("beauty", Compression::ZIP1, &[("R", beauty[0].clone()), ("G", beauty[1].clone()), ("B", beauty[2].clone()), ("A", beauty[3].clone())]),
        layer_f32("diffuse", Compression::RLE, &[("R", diffuse[0].clone()), ("G", diffuse[1].clone()), ("B", diffuse[2].clone())]),
    ])
}

fn stereo_file() -> Vec<u8> {
    let n = 4;
    let px = |k: f32| -> [Vec<f32>; 4] { std::array::from_fn(|c| (0..n).map(|i| if c == 0 { i as f32 / 3.0 * k } else { k }).collect()) };
    let with_view = |name: &str, view: &str, p: [Vec<f32>; 4]| {
        let attrs = LayerAttributes { view_name: Some(view.into()), ..LayerAttributes::named(name) };
        let list: Vec<_> = ["R", "G", "B", "A"].iter().enumerate().map(|(c, cn)| AnyChannel::new(*cn, FlatSamples::F32(p[c].clone()))).collect();
        Layer::new((2, 2), attrs, enc(Compression::Uncompressed), AnyChannels::sort(list.into_iter().collect()))
    };
    write_image(vec![with_view("beauty_left", "left", px(0.1)), with_view("beauty_right", "right", px(0.9))])
}

fn single_y() -> Vec<u8> {
    let v: Vec<f32> = (0..4).map(|i| i as f32 / 3.0).collect();
    write_image(vec![layer_f32("main", Compression::ZIP1, &[("Y", v)])])
}

fn half_rgb() -> Vec<u8> {
    let chans: [Vec<f16>; 3] = std::array::from_fn(|c| (0..4).map(|_| f16::from_f32([0.5, 0.25, 1.0][c])).collect());
    let list: Vec<_> = ["R", "G", "B"].iter().enumerate().map(|(c, cn)| AnyChannel::new(*cn, FlatSamples::F16(chans[c].clone()))).collect();
    write_image(vec![Layer::new((2, 2), LayerAttributes::named("half"), enc(Compression::Uncompressed), AnyChannels::sort(list.into_iter().collect()))])
}

fn text(s: &str) -> Text {
    Text::new_or_panic(s)
}

fn cryptomatte_file(name: &str) -> Vec<u8> {
    let (w, h) = (4usize, 4usize);
    let cube = cryptomatte_id("cube");
    let sphere = cryptomatte_id("sphere");
    let s0: Vec<f32> = vec![cube; w * h];
    let s0c: Vec<f32> = vec![0.6; w * h];
    let s1: Vec<f32> = (0..w * h).map(|p| if p % w == 0 { sphere } else { 0.0 }).collect();
    let s1c: Vec<f32> = (0..w * h).map(|p| if p % w == 0 { 0.25 } else { 0.0 }).collect();
    let mk = |n: String, v: Vec<f32>| AnyChannel::new(n.as_str(), FlatSamples::F32(v));
    let list = vec![mk(format!("{name}00.red"), s0), mk(format!("{name}00.green"), s0c), mk(format!("{name}01.red"), s1), mk(format!("{name}01.green"), s1c)];
    let key = cryptomatte_key("CryptoAsset");
    let manifest = format!("{{\"cube\":\"{:08x}\",\"sphere\":\"{:08x}\"}}", cube.to_bits(), sphere.to_bits());
    let mut attrs = LayerAttributes::named(name);
    attrs.other.insert(text(&format!("cryptomatte/{key}/name")), AttributeValue::Text(text("CryptoAsset")));
    attrs.other.insert(text(&format!("cryptomatte/{key}/hash")), AttributeValue::Text(text(&key)));
    attrs.other.insert(text(&format!("cryptomatte/{key}/conversion")), AttributeValue::Text(text("uint32_to_float32")));
    attrs.other.insert(text(&format!("cryptomatte/{key}/manifest")), AttributeValue::Text(text(&manifest)));
    let layer = Layer::new((w, h), attrs, enc(Compression::Uncompressed), AnyChannels::sort(list.into_iter().collect()));
    write_image(vec![layer])
}

fn cryptomatte_file_short_suffix() -> Vec<u8> {
    let (w, h) = (4usize, 4usize);
    let id = cryptomatte_id("cube");
    let mk = |n: String, v: Vec<f32>| AnyChannel::new(n.as_str(), FlatSamples::F32(v));
    let list = vec![mk("crypto_asset00.r".into(), vec![id; w * h]), mk("crypto_asset00.g".into(), vec![1.0; w * h])];
    let key = cryptomatte_key("CryptoAsset");
    let mut attrs = LayerAttributes::named("crypto_asset");
    attrs.other.insert(text(&format!("cryptomatte/{key}/name")), AttributeValue::Text(text("CryptoAsset")));
    write_image(vec![Layer::new((w, h), attrs, enc(Compression::Uncompressed), AnyChannels::sort(list.into_iter().collect()))])
}

fn plain_rgb() -> Vec<u8> {
    let (w, h) = (2usize, 2usize);
    let mk = |n: &str, v: f32| AnyChannel::new(n, FlatSamples::F32(vec![v; w * h]));
    let list = vec![mk("R", 0.5), mk("G", 0.5), mk("B", 0.5)];
    write_image(vec![Layer::new((w, h), LayerAttributes::named("beauty"), enc(Compression::Uncompressed), AnyChannels::sort(list.into_iter().collect()))])
}

/// FITS header cards (each padded to 80 characters), END, the block padded to 2880.
fn fits_header(cards: &[&str]) -> Vec<u8> {
    let mut out = Vec::new();
    for c in cards.iter().chain(std::iter::once(&"END")) {
        let mut card = c.as_bytes().to_vec();
        card.resize(80, b' ');
        out.extend(card);
    }
    out.resize(out.len().div_ceil(2880) * 2880, b' ');
    out
}

fn fits_u16_bzero() -> Vec<u8> {
    let mut b = fits_header(&[
        "SIMPLE  =                    T",
        "BITPIX  =                   16",
        "NAXIS   =                    2",
        "NAXIS1  =                    3",
        "NAXIS2  =                    2",
        "BZERO   =                32768",
        "OBJECT  = 'M 31    '",
        "EXPTIME =                300.0 / seconds",
        "HISTORY stacked from 12 frames",
        "HIERARCH ESO DET CHIP NAME = 'chip'",
    ]);
    for v in [0u16, 1000, 40000, 65535, 7, 32768] {
        b.extend_from_slice(&((v as i32 - 32768) as i16).to_be_bytes());
    }
    b.resize(b.len().div_ceil(2880) * 2880, 0);
    b
}

fn fits_f32_rgb() -> Vec<u8> {
    let mut b = fits_header(&["SIMPLE  = T", "BITPIX  = -32", "NAXIS   = 3", "NAXIS1  = 2", "NAXIS2  = 2", "NAXIS3  = 3", "ROWORDER= 'TOP-DOWN'"]);
    for i in 0..12 {
        let v = [0.25f32, 65535.0, f32::NAN, -0.5, 1.5, 0.0, 0.125, 1.0, 0.75, 1e-3, 0.5, 0.9][i];
        b.extend_from_slice(&v.to_be_bytes());
    }
    b
}

fn fits_i32_extension() -> Vec<u8> {
    let mut b = fits_header(&["SIMPLE  = T", "BITPIX  = 8", "NAXIS   = 0", "EXTEND  = T", "TELESCOP= 'Seestar S50'"]);
    b.extend(fits_header(&["XTENSION= 'IMAGE   '", "BITPIX  = 32", "NAXIS   = 2", "NAXIS1  = 2", "NAXIS2  = 2", "PCOUNT  = 0", "GCOUNT  = 1", "BSCALE  = 2.0", "BZERO   = 10.0"]));
    for v in [-5i32, 0, 100, 70000] {
        b.extend_from_slice(&v.to_be_bytes());
    }
    b
}

fn pcx(bits: u8, planes: u8, width: u16, height: u16, stride: u16) -> Vec<u8> {
    let mut b = vec![0u8; 128];
    b[0] = 0x0a;
    b[1] = 5;
    b[2] = 1;
    b[3] = bits;
    b[8..10].copy_from_slice(&(width - 1).to_le_bytes());
    b[10..12].copy_from_slice(&(height - 1).to_le_bytes());
    b[12..14].copy_from_slice(&300u16.to_le_bytes());
    b[14..16].copy_from_slice(&150u16.to_le_bytes());
    for i in 0..48 {
        b[16 + i] = (i * 37 % 256) as u8;
    }
    b[65] = planes;
    b[66..68].copy_from_slice(&stride.to_le_bytes());
    b[68..70].copy_from_slice(&1u16.to_le_bytes());
    b
}

fn rle(out: &mut Vec<u8>, row: &[u8]) {
    let mut i = 0;
    while i < row.len() {
        let mut run = 1;
        while i + run < row.len() && row[i + run] == row[i] && run < 63 {
            run += 1;
        }
        if run > 1 || row[i] >= 0xc0 {
            out.extend_from_slice(&[0xc0 | run as u8, row[i]]);
        } else {
            out.push(row[i]);
        }
        i += run;
    }
}

fn pcx_rgb24() -> Vec<u8> {
    let (w, h) = (5u16, 3u16);
    let mut b = pcx(8, 3, w, h, 6);
    for y in 0..h {
        for c in 0..3u16 {
            let row: Vec<u8> = (0..6u16).map(|x| ((x * 50 + y * 30 + c * 70) % 256) as u8).collect();
            rle(&mut b, &row);
        }
    }
    b
}

fn pcx_vga() -> Vec<u8> {
    let (w, h) = (6u16, 2u16);
    let mut b = pcx(8, 1, w, h, 6);
    for y in 0..h {
        let row: Vec<u8> = (0..6u16).map(|x| if x < 3 { 200 } else { (x * 7 + y) as u8 }).collect();
        rle(&mut b, &row);
    }
    b.push(0x0c);
    b.extend((0..768).map(|i| (i * 13 % 256) as u8));
    b
}

fn pcx_planar(bits: u8, planes: u8) -> Vec<u8> {
    let (w, h) = (9u16, 3u16);
    let stride = (w * u16::from(bits)).div_ceil(16) * 2;
    let mut b = pcx(bits, planes, w, h, stride);
    for y in 0..h {
        for p in 0..u16::from(planes) {
            let row: Vec<u8> = (0..stride).map(|x| ((x * 91 + y * 17 + p * 29) % 256) as u8).collect();
            rle(&mut b, &row);
        }
    }
    b
}

/// FITS files written by the reference's encoder, compared byte for byte by test_new_formats.c.
fn fits_written(kind: u8) -> Vec<u8> {
    use photocraft_codecs::{ChannelLayout, EncodeOptions, Format, Image, encode};
    let mut img = match kind {
        0 => Image::from_u16(3, 2, ChannelLayout::Gray, &[0, 1000, 20000, 40000, 65535, 7]).unwrap(),
        1 => Image::from_f32(2, 2, ChannelLayout::Rgb, &[0.0, 0.5, 1.0, 0.25, 0.75, 0.125, 1.5, -0.25, 0.3, 0.6, 0.9, 0.05]).unwrap(),
        _ => Image::from_u8(2, 1, ChannelLayout::Rgba, vec![10, 20, 30, 40, 50, 60, 70, 80]).unwrap(),
    };
    img.meta.text = vec![
        ("OBSERVER".into(), "O'Brien".into()),
        ("Software".into(), "photocraft 1.0".into()),
        ("HISTORY".into(), "stacked".into()),
        ("EXPTIME".into(), "300.5".into()),
        ("Comment".into(), "a long comment that does not fit the eighty columns of one card, so it wraps".into()),
    ];
    img.meta.dpi = Some((300.0, 300.0));
    encode(&img, Format::Fits, &EncodeOptions::default()).unwrap()
}

pub fn codecs_fixtures(dir: &str) {
    let files: [(&str, Vec<u8>); 17] = [
        ("fits_u16_bzero.fits", fits_u16_bzero()),
        ("fits_f32_rgb.fits", fits_f32_rgb()),
        ("fits_i32_extension.fits", fits_i32_extension()),
        ("fits_written_u16.fits", fits_written(0)),
        ("fits_written_f32_rgb.fits", fits_written(1)),
        ("fits_written_u8_rgba.fits", fits_written(2)),
        ("pcx_rgb24.pcx", pcx_rgb24()),
        ("pcx_vga.pcx", pcx_vga()),
        ("pcx_4bit.pcx", pcx_planar(4, 1)),
        ("pcx_1bit_4planes.pcx", pcx_planar(1, 4)),
        ("aov.exr", aov_file()),
        ("stereo.exr", stereo_file()),
        ("single_y.exr", single_y()),
        ("half_rgb.exr", half_rgb()),
        ("crypto_asset.exr", cryptomatte_file("crypto_asset")),
        ("crypto_short.exr", cryptomatte_file_short_suffix()),
        ("plain_rgb.exr", plain_rgb()),
    ];
    for (name, bytes) in files {
        let path = format!("{dir}/{name}");
        std::fs::write(&path, bytes).unwrap_or_else(|e| panic!("{path}: {e}"));
        println!("{path}");
    }
}
