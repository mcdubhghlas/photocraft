//! C4 format container/store reference vectors (`rustref format-vectors`), checked by
//! tests/format/test_parity.c. Lines are tab-separated; strings are Rust `{:?}`.
//!
//!   crc DESC CRC                       zip::crc32
//!   zipw CASE LEN BLAKE3 [HEX]         ZipWriter output (STORE: the bytes must be identical)
//!   zipw-limit RESULT                  the 65536th entry
//!   zipr CASE HEX SUMMARY              ZipReader over a synthetic or hostile archive
//!   base HEX                           the archive the sweeps below mutate
//!   trunc CUT SUMMARY                  base[..CUT]
//!   mut POS XOR SUMMARY                base with one byte xor-ed
//!   ispc HEX BOOL                      is_pcraft
//!   bufferfor N RESULT                 read::buffer_for
//!   tmpname INPUT OUTPUT               atomic::temp_name with the pid and counter as PID and N
//!   bundle NAME STATS LEN BASE64       PcraftWriter::save_zip of a synthetic document
//!   tile NAME PATH LEN                 uncompressed length of each tile object
//!   dir NAME STATS LISTING             PcraftWriter::save_dir: files written (sorted)
//!   doc-* ...                          document-level results (C5 checks them)
//!   xload NAME CASE KIND LEN ERROR     load_from_bytes after one object was damaged
//!   xlimit WHAT VALUE ERROR            load_from_bytes_with a tight limit
//!   xdepth CASE ERROR                  load_from_bytes with a deep or tricky manifest
//!
//! SUMMARY: `pc=0|1` then `E"error"` or `okN` and, per entry, ` "name"=R1/R2` with R1 = read
//! at most 2^30 bytes and R2 = at most 3 bytes (`LEN:BLAKE3-16` or `E"error"`), then
//! ` rbn=R` for read_by_name("manifest.json").
//!
//! Cross-direction check: with PC_FORMAT_XREAD=DIR set, also loads DIR/NAME.pcraft and DIR/NAME.dir
//! (bundles the C writer produced, see test_parity.c) and prints `xread NAME KIND RESULT`.
use std::sync::Arc;

use base64::Engine;
use photocraft_doc::{Color, ColorMode, Document, Layer, LayerMask, SampleType, Size, Surface};
use photocraft_format::zip::{crc32, ZipReader, ZipWriter};
use photocraft_format::{self as pf, LoadOptions, PcraftWriter, SaveOptions, SaveStats};
use photocraft_raster::Rgba8Image;

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn b3(b: &[u8]) -> String {
    blake3::hash(b).to_hex()[..16].to_string()
}

fn rd(r: &ZipReader, e: &pf::zip::Entry, max: usize) -> String {
    match r.read(e, max) {
        Ok(d) => format!("{}:{}", d.len(), b3(&d)),
        Err(e) => format!("E{:?}", e.to_string()),
    }
}

fn zsum(b: &[u8]) -> String {
    let pc = pf::is_pcraft(b) as u8;
    match ZipReader::new(b) {
        Err(e) => format!("pc={pc} E{:?}", e.to_string()),
        Ok(r) => {
            let mut s = format!("pc={pc} ok{}", r.entries.len());
            for e in &r.entries {
                s += &format!(" {:?}={}/{}", e.name, rd(&r, e, 1 << 30), rd(&r, e, 3));
            }
            s += &format!(
                " rbn={}",
                match r.read_by_name("manifest.json", 1 << 30) {
                    Ok(d) => format!("{}:{}", d.len(), b3(&d)),
                    Err(e) => format!("E{:?}", e.to_string()),
                }
            );
            s
        }
    }
}

/// One entry of a hand-built archive (every field under the caller's control).
#[derive(Clone)]
struct Re {
    name: Vec<u8>,
    method: u16,
    flags: u16,
    crc: u32,
    csize: u32,
    usize_: u32,
    data: Vec<u8>,
    lname: Option<Vec<u8>>,
    lextra: Vec<u8>,
    cextra: Vec<u8>,
    ccomment: Vec<u8>,
    off: Option<u32>,
}

fn re(name: &[u8], method: u16, data: &[u8], plain: &[u8]) -> Re {
    Re {
        name: name.to_vec(),
        method,
        flags: 0x0800,
        crc: crc32(plain),
        csize: data.len() as u32,
        usize_: plain.len() as u32,
        data: data.to_vec(),
        lname: None,
        lextra: Vec::new(),
        cextra: Vec::new(),
        ccomment: Vec::new(),
        off: None,
    }
}

fn stored(name: &[u8], data: &[u8]) -> Re {
    re(name, 0, data, data)
}

/// Builds an archive; `count` overrides the EOCD entry count, `comment` follows the EOCD.
fn build(entries: &[Re], count: Option<u16>, comment: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut central = Vec::new();
    for e in entries {
        let off = out.len() as u32;
        let lname = e.lname.clone().unwrap_or_else(|| e.name.clone());
        out.extend_from_slice(&0x0403_4b50u32.to_le_bytes());
        out.extend_from_slice(&20u16.to_le_bytes());
        out.extend_from_slice(&e.flags.to_le_bytes());
        out.extend_from_slice(&e.method.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(&0x21u16.to_le_bytes());
        out.extend_from_slice(&e.crc.to_le_bytes());
        out.extend_from_slice(&e.csize.to_le_bytes());
        out.extend_from_slice(&e.usize_.to_le_bytes());
        out.extend_from_slice(&(lname.len() as u16).to_le_bytes());
        out.extend_from_slice(&(e.lextra.len() as u16).to_le_bytes());
        out.extend_from_slice(&lname);
        out.extend_from_slice(&e.lextra);
        out.extend_from_slice(&e.data);
        central.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
        central.extend_from_slice(&20u16.to_le_bytes());
        central.extend_from_slice(&20u16.to_le_bytes());
        central.extend_from_slice(&e.flags.to_le_bytes());
        central.extend_from_slice(&e.method.to_le_bytes());
        central.extend_from_slice(&0u16.to_le_bytes());
        central.extend_from_slice(&0x21u16.to_le_bytes());
        central.extend_from_slice(&e.crc.to_le_bytes());
        central.extend_from_slice(&e.csize.to_le_bytes());
        central.extend_from_slice(&e.usize_.to_le_bytes());
        central.extend_from_slice(&(e.name.len() as u16).to_le_bytes());
        central.extend_from_slice(&(e.cextra.len() as u16).to_le_bytes());
        central.extend_from_slice(&(e.ccomment.len() as u16).to_le_bytes());
        central.extend_from_slice(&[0; 8]);
        central.extend_from_slice(&e.off.unwrap_or(off).to_le_bytes());
        central.extend_from_slice(&e.name);
        central.extend_from_slice(&e.cextra);
        central.extend_from_slice(&e.ccomment);
    }
    let cd_off = out.len() as u32;
    let n = count.unwrap_or(entries.len() as u16);
    out.extend_from_slice(&central);
    out.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
    out.extend_from_slice(&[0; 4]);
    out.extend_from_slice(&n.to_le_bytes());
    out.extend_from_slice(&n.to_le_bytes());
    out.extend_from_slice(&(central.len() as u32).to_le_bytes());
    out.extend_from_slice(&cd_off.to_le_bytes());
    out.extend_from_slice(&(comment.len().min(65535) as u16).to_le_bytes());
    out.extend_from_slice(comment);
    out
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

const HELLO: &[u8] = b"hello hello hello hello deflate world!";
const HELLO_DEFLATE: &str = "cb48cdc9c957c8c0205352d372124b5215caf38b72521401";
const A5000_DEFLATE: &str = "edc13101000000c2a06ceb5fca147e4001000000006f03";

fn writer(entries: &[(&str, &[u8])]) -> Vec<u8> {
    let mut w = ZipWriter::new();
    for (n, d) in entries {
        w.add(n, d).unwrap();
    }
    w.finish().unwrap()
}

/// Patch little-endian bytes at `at`.
fn patch(mut b: Vec<u8>, at: usize, v: &[u8]) -> Vec<u8> {
    b[at..at + v.len()].copy_from_slice(v);
    b
}

fn eocd(b: &[u8]) -> usize {
    (0..=b.len() - 22).rev().find(|&i| b[i..i + 4] == [0x50, 0x4b, 5, 6]).unwrap()
}

fn zip_vectors() {
    // CRC-32.
    let pattern: Vec<u8> = (0..1_000_003u32).map(|i| (i.wrapping_mul(2_654_435_761) >> 13) as u8).collect();
    let all: Vec<u8> = (0..=255u8).collect();
    for (desc, data) in [("empty", &b""[..]), ("123456789", b"123456789"), ("bytes0-255", &all), ("pattern1000003", &pattern)] {
        println!("crc\t{desc}\t{:08x}", crc32(data));
    }

    // Writer bytes.
    let a5000 = vec![b'A'; 5000];
    let wcases: Vec<(&str, Vec<u8>)> = vec![
        ("none", writer(&[])),
        ("one", writer(&[("manifest.json", b"{}")])),
        ("three", writer(&[("a.txt", b"hello"), ("dir/b.bin", &[0, 1, 2, 3]), ("empty", &[])])),
        ("utf8", writer(&[("t\u{00e9}st/\u{2603}.zst", &all), ("big", &pattern[..70_000])])),
        ("long", writer(&[(&"n".repeat(300), &a5000)])),
    ];
    for (name, b) in &wcases {
        let h = if b.len() <= 200 { format!("\t{}", hex(b)) } else { String::new() };
        println!("zipw\t{name}\t{}\t{}{h}", b.len(), blake3::hash(b).to_hex());
    }
    let mut w = ZipWriter::new();
    for i in 0..65535u32 {
        w.add(if i == 0 { "first" } else { "" }, &[]).unwrap();
    }
    let r = match w.add("over", &[]) {
        Ok(()) => "ok".to_owned(),
        Err(e) => format!("E{:?}", e.to_string()),
    };
    let b = w.finish().unwrap();
    println!("zipw-limit\t{r}\t{}\t{}", b.len(), blake3::hash(&b).to_hex());

    // Reader cases.
    let hello_d = unhex(HELLO_DEFLATE);
    let a_d = unhex(A5000_DEFLATE);
    let basic = writer(&[("manifest.json", b"{}"), ("a.txt", b"hello"), ("dir/b.bin", &[0, 1, 2, 3]), ("empty", &[])]);
    let mut cases: Vec<(&str, Vec<u8>)> = vec![
        ("empty-archive", writer(&[])),
        ("short", vec![0; 21]),
        ("no-eocd", vec![b'x'; 100]),
        ("basic", basic.clone()),
        ("deflate", build(&[re(b"manifest.json", 8, &hello_d, HELLO)], None, b"")),
        ("deflate-big", build(&[re(b"a", 8, &a_d, &a5000)], None, b"")),
    ];
    let mut under = re(b"a", 8, &a_d, &a5000);
    under.usize_ = 10;
    cases.push(("deflate-underdeclared", build(&[under], None, b"")));
    let mut over = re(b"a", 8, &hello_d, HELLO);
    over.usize_ = 1000;
    cases.push(("deflate-overdeclared", build(&[over], None, b"")));
    cases.push(("deflate-corrupt", build(&[re(b"a", 8, &[0xff, 0xff, 0xff, 0xff], HELLO)], None, b"")));
    cases.push(("deflate-truncated", build(&[re(b"a", 8, &hello_d[..hello_d.len() / 2], HELLO)], None, b"")));
    cases.push(("deflate-empty", build(&[re(b"a", 8, &[], b"")], None, b"")));
    cases.push(("deflate-stored-block", build(&[re(b"a", 8, &[1, 5, 0, 0xfa, 0xff, b'h', b'e', b'l', b'l', b'o'], b"hello")], None, b"")));
    cases.push(("deflate-trailing", build(&[re(b"a", 8, &[&hello_d[..], b"junkjunk"].concat(), HELLO)], None, b"")));
    cases.push(("method12", build(&[re(b"x.bz", 12, b"BZh", b"BZh")], None, b"")));
    cases.push(("method1-empty", build(&[re(b"e", 1, b"", b"")], None, b"")));
    let mut enc = stored(b"secret", b"data");
    enc.flags = 0x0801;
    cases.push(("encrypted", build(&[stored(b"ok", b"fine"), enc], None, b"")));
    let mut badcrc = stored(b"manifest.json", b"hello");
    badcrc.crc ^= 1;
    cases.push(("crc-bad", build(&[badcrc], None, b"")));
    let mut sz = stored(b"a", b"hello");
    sz.usize_ = 4;
    cases.push(("store-size-mismatch", build(&[sz], None, b"")));
    let mut sz2 = stored(b"a", b"hello");
    sz2.usize_ = 6;
    cases.push(("store-size-larger", build(&[sz2], None, b"")));
    let e = eocd(&basic);
    let cdoff = u32::from_le_bytes(basic[e + 16..e + 20].try_into().unwrap()) as usize;
    let cdsize = u32::from_le_bytes(basic[e + 12..e + 16].try_into().unwrap());
    cases.push(("cd-out-of-range", patch(basic.clone(), e + 12, &(cdsize + 1).to_le_bytes())));
    cases.push(("cd-offset-max", patch(basic.clone(), e + 16, &u32::MAX.to_le_bytes())));
    cases.push(("cd-size-max", patch(basic.clone(), e + 12, &u32::MAX.to_le_bytes())));
    cases.push(("count-too-big", patch(basic.clone(), e + 10, &5u16.to_le_bytes())));
    cases.push(("count-65535", patch(basic.clone(), e + 10, &65535u16.to_le_bytes())));
    cases.push(("count-zero", patch(basic.clone(), e + 10, &0u16.to_le_bytes())));
    cases.push(("count-smaller", patch(basic.clone(), e + 10, &2u16.to_le_bytes())));
    cases.push(("bad-cd-sig", patch(basic.clone(), cdoff, b"PK\x01\x03")));
    cases.push(("name-out-of-range", patch(basic.clone(), cdoff + 28, &4000u16.to_le_bytes())));
    cases.push(("extra-out-of-range", patch(basic.clone(), cdoff + 30, &4000u16.to_le_bytes())));
    cases.push(("comment-out-of-range", patch(basic.clone(), cdoff + 32, &4000u16.to_le_bytes())));
    cases.push(("bad-local-sig", patch(basic.clone(), 0, b"PK\x03\x05")));
    cases.push(("local-beyond", patch(basic.clone(), cdoff + 42, &u32::MAX.to_le_bytes())));
    cases.push(("local-in-cd", patch(basic.clone(), cdoff + 42, &(cdoff as u32).to_le_bytes())));
    cases.push(("csize-huge", patch(basic.clone(), cdoff + 20, &0xffff_fff0u32.to_le_bytes())));
    cases.push(("usize-huge", patch(basic.clone(), cdoff + 24, &0xffff_fff0u32.to_le_bytes())));
    cases.push(("local-name-len-huge", patch(basic.clone(), 26, &0xffffu16.to_le_bytes())));
    cases.push(("local-extra-shift", patch(basic.clone(), 28, &3u16.to_le_bytes())));
    // A local header whose fields run past the end: its signature sits in the archive comment.
    let mut tl = stored(b"t", b"");
    let probe = build(&[tl.clone()], None, b"PK\x03\x04xx");
    tl.off = Some((probe.len() - 6) as u32);
    cases.push(("local-header-truncated", build(&[tl], None, b"PK\x03\x04xx")));
    // Two directory entries sharing one local header (overlapping data).
    let mut ov = stored(b"second", b"shared");
    ov.off = Some(0);
    cases.push(("overlap", build(&[stored(b"first", b"shared"), ov], None, b"")));
    cases.push(("comment", build(&[stored(b"manifest.json", b"{}")], None, &[b'c'; 100])));
    let mut fake = b"PK\x05\x06".to_vec();
    fake.extend_from_slice(&[0; 18]);
    cases.push(("fake-eocd-in-comment", build(&[stored(b"manifest.json", b"{}")], None, &fake)));
    let mut fake2 = b"PK\x05\x06".to_vec();
    fake2.extend_from_slice(&[0, 0, 0, 0, 1, 0, 1, 0, 46, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
    cases.push(("fake-eocd-bad", build(&[stored(b"manifest.json", b"{}")], None, &fake2)));
    cases.push(("eocd-trailing-junk", [build(&[stored(b"a", b"x")], None, b""), b"tail".to_vec()].concat()));
    let mut bad = stored(&[0x66, 0xff, 0x80, 0x67], b"x");
    bad.csize = 999;
    cases.push(("invalid-utf8-name", build(&[stored(&[0xc3, 0xa9, 0xe2, 0x82], b"y"), bad], None, b"")));
    cases.push(("traversal-name", build(&[stored(b"../../etc/passwd", b"root"), stored(b"/abs/manifest.json", b"{}")], None, b"")));
    cases.push(("dup-names", build(&[stored(b"manifest.json", b"first"), stored(b"manifest.json", b"second!")], None, b"")));
    cases.push(("nul-name", build(&[stored(b"a\0b", b"z"), stored(b"manifest.json\0", b"q")], None, b"")));
    let mut ex = stored(b"manifest.json", b"{}");
    ex.cextra = vec![1, 2, 3, 4, 5];
    ex.ccomment = b"note".to_vec();
    ex.lextra = vec![9; 7];
    cases.push(("extras", build(&[ex, stored(b"b", b"bb")], None, b"")));
    let mut ln = stored(b"manifest.json", b"{}");
    ln.lname = Some(b"m".to_vec());
    cases.push(("local-name-differs", build(&[ln], None, b"")));
    cases.push(("pk-only", b"PK\x03\x04".to_vec()));
    let mut cdfirst = b"PK\x05\x06".to_vec();
    cdfirst.extend_from_slice(&[0; 18]);
    cases.push(("eocd-only-in-prefix", [cdfirst, vec![b'z'; 30]].concat()));
    for (name, b) in &cases {
        println!("zipr\t{name}\t{}\t{}", hex(b), zsum(b));
    }

    // Sweeps over one archive with STORE and DEFLATE entries.
    let base = build(
        &[stored(b"manifest.json", b"{\"v\":1}"), re(b"d", 8, &hello_d, HELLO), stored(b"tiles/e.zst", b""), re(b"z", 8, &a_d, &a5000)],
        None,
        b"",
    );
    println!("base\t{}", hex(&base));
    for cut in 0..base.len() {
        println!("trunc\t{cut}\t{}", zsum(&base[..cut]));
    }
    for pos in 0..base.len() {
        for x in [0x01u8, 0xff] {
            let mut m = base.clone();
            m[pos] ^= x;
            println!("mut\t{pos}\t{x}\t{}", zsum(&m));
        }
    }

    // is_pcraft.
    let mut first30 = b"PK\x03\x04".to_vec();
    first30.extend_from_slice(&[0; 22]);
    first30.extend_from_slice(&13u16.to_le_bytes());
    first30.extend_from_slice(&[0, 0]);
    let mut header_only = first30.clone();
    header_only.extend_from_slice(b"manifest.json");
    let mut short_name = first30.clone();
    short_name.extend_from_slice(b"manifest.jso");
    for b in [
        Vec::new(),
        b"PK\x03\x04".to_vec(),
        header_only,
        short_name,
        writer(&[("a", b"1"), ("manifest.json", b"{}")]),
        writer(&[("a", b"1")]),
        writer(&[("manifest.json", b"")]),
        b"PK\x05\x06\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0".to_vec(),
    ] {
        println!("ispc\t{}\t{}", hex(&b), pf::is_pcraft(&b));
    }
}

fn read_vectors() {
    for n in [u64::MAX, 1u64 << 63, (1u64 << 63) + 12_345, 1u64 << 62] {
        let r = match pf::read::buffer_for(n) {
            Ok(b) => format!("ok {}", b.capacity()),
            Err(e) => format!("E{:?} {:?}", e.kind(), e.to_string()),
        };
        println!("bufferfor\t{n}\t{r}");
    }
    for n in [0u64, 1, 1024] {
        let r = match pf::read::buffer_for(n) {
            Ok(b) => format!("ok {}", b.capacity()),
            Err(e) => format!("E{:?} {:?}", e.kind(), e.to_string()),
        };
        println!("bufferfor\t{n}\t{r}");
    }
}

fn atomic_vectors() {
    let pid = std::process::id();
    let long: String = "x".repeat(100);
    let multi: String = "\u{00fc}\u{2603}".repeat(40);
    for input in ["doc.psd", "", "a b.pcraft", long.as_str(), multi.as_str()] {
        let t = pf::atomic::temp_name(input);
        // ".{stem}.{pid}-{n}.pcsave.tmp": hide the pid and the process-wide counter.
        let marker = format!(".{pid}-");
        let i = t.rfind(&marker).unwrap();
        let rest = &t[i + marker.len()..];
        let j = rest.find('.').unwrap();
        println!("tmpname\t{input:?}\t{:?}", format!("{}.PID-N{}", &t[..i], &rest[j..]));
    }
}

struct Rng(u64);
impl Rng {
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
    let mut r = Rng(seed);
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

fn docs() -> Vec<(&'static str, Document, SaveOptions)> {
    let mut out = Vec::new();
    // RGB 8-bit: background, two layers with the same pixels (deduplicated tiles), a mask, blobs, previews.
    let mut d = Document::with_background("Rgb8", Size::new(300, 280), ColorMode::Rgb, SampleType::U8, Color::WHITE);
    d.icc_profile = Some(blob(1, 500));
    let pf_ = d.pixel_format();
    let mut paint = Layer::raster("Paint", pf_);
    scribble(paint.surface_mut().unwrap(), 1, false);
    let mut mask = LayerMask::hide_all();
    scribble(&mut mask.surface, 2, false);
    paint.mask = Some(mask);
    paint.psd_blocks.push((*b"lnsr", blob(5, 4)));
    let mut twin = Layer::raster("Twin", pf_);
    scribble(twin.surface_mut().unwrap(), 1, false);
    twin.psd_blocks.push((*b"lnsr", blob(5, 4)));
    d.layers.push(paint);
    d.layers.push(twin);
    let thumb = Rgba8Image { width: 4, height: 3, pixels: (0..48).map(|i| (i * 5) as u8).collect() };
    let comp = Rgba8Image { width: 2, height: 2, pixels: vec![255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 9, 9, 9, 9] };
    out.push(("rgb8", d, SaveOptions { thumbnail: Some(thumb), composite: Some(comp) }));

    let mut d = Document::with_background("Gray16", Size::new(260, 10), ColorMode::Grayscale, SampleType::U16, Color::BLACK);
    let mut l = Layer::raster("G", d.pixel_format());
    scribble(l.surface_mut().unwrap(), 7, false);
    d.layers.push(l);
    out.push(("gray16", d, SaveOptions::default()));

    let mut d = Document::new("Float", Size::new(64, 64), ColorMode::Rgb, SampleType::F32);
    let mut l = Layer::raster("Hdr", d.pixel_format());
    scribble(l.surface_mut().unwrap(), 9, true);
    d.layers.push(l);
    d.icc_profile = Some(blob(2, 3000));
    out.push(("rgbf32", d, SaveOptions::default()));

    out.push(("empty", Document::new("Empty", Size::new(1, 1), ColorMode::Rgb, SampleType::U8), SaveOptions::default()));
    out
}

fn stats(s: &SaveStats) -> String {
    format!(
        "{},{},{},{},{},{},{}",
        s.tiles_total, s.tiles_written, s.tiles_reused, s.blobs_total, s.blobs_written, s.objects_removed, s.manifest_bytes
    )
}

/// Uncompressed tile lengths by bundle path (blake3 of the little-endian tile bytes).
fn tile_lens(d: &Document) -> std::collections::BTreeMap<String, usize> {
    let mut m = std::collections::BTreeMap::new();
    let mut add = |s: &Surface| {
        for (_, t) in s.tiles() {
            m.insert(format!("tiles/{}.zst", blake3::hash(t.bytes()).to_hex()), t.bytes().len());
        }
    };
    for l in &d.layers {
        if let Some(s) = l.surface() {
            add(s);
        }
        if let Some(mk) = &l.mask {
            add(&mk.surface);
        }
    }
    m
}

fn temp_dir(tag: &str) -> std::path::PathBuf {
    let d = std::env::temp_dir().join(format!("rustref-format-{}-{tag}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn listing(root: &std::path::Path) -> String {
    let mut v = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(p) = stack.pop() {
        for e in std::fs::read_dir(&p).unwrap() {
            let e = e.unwrap();
            let rel = e.path().strip_prefix(root).unwrap().to_string_lossy().into_owned();
            if e.file_type().unwrap().is_dir() {
                v.push(format!("{rel}/"));
                stack.push(e.path());
            } else {
                v.push(rel);
            }
        }
    }
    v.sort();
    v.join(",")
}

fn err_s(r: Result<Document, pf::FormatError>) -> String {
    match r {
        Ok(_) => "ok".to_owned(),
        Err(e) => format!("E{:?}", e.to_string()),
    }
}

/// Rewrites a bundle, replacing (Some) or dropping (None) entries by name.
fn rebuild(bytes: &[u8], f: impl Fn(&str, Vec<u8>) -> Option<Vec<u8>>) -> Vec<u8> {
    let r = ZipReader::new(bytes).unwrap();
    let mut z = ZipWriter::new();
    for e in &r.entries {
        if let Some(d) = f(&e.name, r.read(e, 1 << 30).unwrap()) {
            z.add(&e.name, &d).unwrap();
        }
    }
    z.finish().unwrap()
}

fn store_vectors() {
    let mut bundles = Vec::new();
    for (name, doc, opts) in docs() {
        let mut w = PcraftWriter::new();
        let (bytes, st) = w.save_zip(&doc, &opts).unwrap();
        println!("bundle\t{name}\t{}\t{}\t{}", stats(&st), bytes.len(), base64::engine::general_purpose::STANDARD.encode(&bytes));
        for (path, len) in tile_lens(&doc) {
            println!("tile\t{name}\t{path}\t{len}");
        }
        let (_, st2) = w.save_zip(&doc, &opts).unwrap();
        println!("doc-stats2\t{name}\t{}", stats(&st2));
        let dir = temp_dir(name);
        let mut wd = PcraftWriter::new();
        let sd = wd.save_dir(&doc, &dir, &opts).unwrap();
        println!("dir\t{name}\t{}\t{}", stats(&sd), listing(&dir));
        let sd2 = wd.save_dir(&doc, &dir, &opts).unwrap();
        println!("doc-dirstats2\t{name}\t{}", stats(&sd2));
        println!("doc-loaddir\t{name}\t{}", if pf::load_path(&dir).ok().as_ref() == Some(&doc) { "equal" } else { "DIFFERENT" });
        println!("doc-load\t{name}\t{}", if pf::load_from_bytes(&bytes).ok().as_ref() == Some(&doc) { "equal" } else { "DIFFERENT" });
        println!("ispc\t{name}\t{}", pf::is_pcraft(&bytes));
        let _ = std::fs::remove_dir_all(&dir);
        bundles.push((name, doc, bytes));
    }

    // One damaged object at a time (the load error names it).
    let (_, rgb_doc, rgb) = &bundles[0];
    let lens = tile_lens(rgb_doc);
    let r = ZipReader::new(rgb).unwrap();
    let tiles: Vec<String> = r.entries.iter().filter(|e| e.name.starts_with("tiles/")).map(|e| e.name.clone()).collect();
    let blobs: Vec<String> = r.entries.iter().filter(|e| e.name.starts_with("blobs/")).map(|e| e.name.clone()).collect();
    let foreign = {
        let r16 = ZipReader::new(&bundles[1].2).unwrap();
        let e = r16.entries.iter().find(|e| e.name.starts_with("tiles/")).unwrap();
        (e.name.clone(), r16.read(e, 1 << 30).unwrap())
    };
    let data = |n: &str| r.read_by_name(n, 1 << 30).unwrap();
    let mut cases: Vec<(String, String, Box<dyn Fn(&str, Vec<u8>) -> Option<Vec<u8>>>)> = Vec::new();
    for list in [&tiles, &blobs] {
        let t = list[0].clone();
        let other = data(&list[1]);
        cases.push((format!("swap {} {}", t, list[1]), t.clone(), Box::new({ let t = t.clone(); move |n, d| Some(if n == t { other.clone() } else { d }) })));
        cases.push((format!("trunc-half {t}"), t.clone(), Box::new({ let t = t.clone(); move |n, d| Some(if n == t { d[..d.len() / 2].to_vec() } else { d }) })));
        cases.push((format!("garbage {t}"), t.clone(), Box::new({ let t = t.clone(); move |n, d| Some(if n == t { b"garbage".to_vec() } else { d }) })));
        cases.push((format!("empty {t}"), t.clone(), Box::new({ let t = t.clone(); move |n, d| Some(if n == t { Vec::new() } else { d }) })));
        cases.push((format!("remove {t}"), t.clone(), Box::new({ let t = t.clone(); move |n, d| (n != t).then_some(d) })));
        for pos in [4usize, 6, 12, 20] {
            let d0 = data(&t);
            if pos < d0.len() {
                cases.push((format!("flip {t} {pos} 90"), t.clone(), Box::new({ let t = t.clone(); move |n, mut d| { if n == t { d[pos] ^= 0x5a; } Some(d) } })));
            }
        }
    }
    let t0 = tiles[0].clone();
    let fdata = foreign.1.clone();
    cases.push((format!("foreign {t0} gray16 {}", foreign.0), t0.clone(), Box::new(move |n, d| Some(if n == t0 { fdata.clone() } else { d }))));
    for (case, target, f) in &cases {
        let b = rebuild(rgb, f);
        let (kind, len) = if target.starts_with("tiles/") { ("tile", lens[target].to_string()) } else { ("blob", "-".to_owned()) };
        println!("xload\trgb8\t{case}\t{kind}\t{len}\t{}", err_s(pf::load_from_bytes(&b)));
    }

    // Limits.
    for v in [100usize, 1000] {
        let o = LoadOptions { max_manifest_bytes: v, ..Default::default() };
        println!("xlimit\tmanifest\t{v}\t{}", err_s(pf::load_from_bytes_with(rgb, &o)));
    }
    for v in [1000u64, 300_000] {
        let o = LoadOptions { max_total_bytes: v, ..Default::default() };
        println!("xlimit\ttotal\t{v}\t{}", err_s(pf::load_from_bytes_with(rgb, &o)));
    }
    for v in [10usize, 499] {
        let o = LoadOptions { max_blob_bytes: v, ..Default::default() };
        println!("xlimit\tblob\t{v}\t{}", err_s(pf::load_from_bytes_with(rgb, &o)));
    }

    // Manifest nesting (the linear depth check runs before any JSON parsing).
    let nested = |n: usize| format!("{}{}", "[".repeat(n), "]".repeat(n)).into_bytes();
    let depth_cases: Vec<(String, Vec<u8>)> = vec![
        ("nested-100000".into(), nested(100_000)),
        ("nested-300".into(), nested(300)),
        ("nested-367".into(), nested(367)),
        ("nested-368".into(), nested(368)),
        ("open-368".into(), "{\"a\":".repeat(368).into_bytes()),
        ("unbalanced-closers".into(), format!("{}{}", "]".repeat(1000), "[".repeat(367)).into_bytes()),
        ("strings".into(), format!(r#"{{"x":"{}\"{}"}}"#, "[".repeat(100_000), "{".repeat(10)).into_bytes()),
        ("escaped-backslash".into(), format!(r#"{{"x":"\\"{}"#, "[".repeat(400)).into_bytes()),
        ("not-json".into(), b"{not json".to_vec()),
    ];
    for (case, m) in &depth_cases {
        let b = rebuild(rgb, |n, d| Some(if n == "manifest.json" { m.clone() } else { d }));
        println!("xdepth\t{case}\t{}", err_s(pf::load_from_bytes(&b)));
    }
    let no_manifest = rebuild(rgb, |n, d| (n != "manifest.json").then_some(d));
    println!("xdepth\tmissing\t{}", err_s(pf::load_from_bytes(&no_manifest)));

    // Cross-direction: bundles the C writer produced from these documents' objects.
    if let Ok(dir) = std::env::var("PC_FORMAT_XREAD") {
        let dir = std::path::PathBuf::from(dir);
        for (name, _, bytes) in &bundles {
            let reference = pf::load_from_bytes(bytes).unwrap();
            for (kind, p) in [("zip", dir.join(format!("{name}.pcraft"))), ("dir", dir.join(format!("{name}.dir")))] {
                let r = match pf::load_path(&p) {
                    Ok(d) if d == reference => "equal".to_owned(),
                    Ok(_) => "DIFFERENT".to_owned(),
                    Err(e) => format!("E{:?}", e.to_string()),
                };
                println!("xread\t{name}\t{kind}\t{r}");
            }
            if let Ok(b) = std::fs::read(dir.join(format!("{name}.pcraft"))) {
                // The non-payload bytes must equal what this ZipWriter writes for the same entries.
                let r = ZipReader::new(&b).unwrap();
                let mut z = ZipWriter::new();
                for e in &r.entries {
                    z.add(&e.name, &r.read(e, 1 << 30).unwrap()).unwrap();
                }
                println!("xread\t{name}\tzip-rewrite\t{}", if z.finish().unwrap() == b { "identical" } else { "DIFFERENT" });
            }
        }
    }
}

pub fn run() {
    zip_vectors();
    read_vectors();
    atomic_vectors();
    store_vectors();
}
