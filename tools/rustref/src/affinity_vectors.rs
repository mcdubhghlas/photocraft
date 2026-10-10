//! `rustref affinity-vectors DIR`: every Affinity document under DIR (sorted) read into the model
//! and hashed field by field, its preview, and seeded truncation / mutation sweeps; checked by
//! tests/affinity/test_parity.c.
use photocraft_affinity::model::{Document, Image, Kind, Node, Path, Pixels};
use photocraft_affinity::paint::{Color, Paint, Stroke};
use photocraft_affinity::{Limits, preview, read};

const H0: u64 = 0xcbf29ce484222325;
fn fnv(h: &mut u64, b: &[u8]) {
    for &x in b {
        *h ^= x as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}
fn u(h: &mut u64, v: u64) { fnv(h, &v.to_le_bytes()); }
fn f(h: &mut u64, v: f64) { fnv(h, &v.to_bits().to_le_bytes()); }
fn s(h: &mut u64, v: &str) { fnv(h, v.as_bytes()); fnv(h, &[0]); }
fn b(h: &mut u64, v: bool) { fnv(h, &[v as u8]); }

fn color(h: &mut u64, c: &Color) {
    match *c {
        Color::Rgb { r, g, b: bl, a } => { fnv(h, &[0]); for v in [r, g, bl, a] { f(h, v); } }
        Color::Cmyk { c, m, y, k, a } => { fnv(h, &[1]); for v in [c, m, y, k, a] { f(h, v); } }
        Color::Gray { v, a } => { fnv(h, &[2]); f(h, v); f(h, a); }
        Color::Lab { l, a, b: bl, alpha } => { fnv(h, &[3]); for v in [l, a, bl, alpha] { f(h, v); } }
    }
}
fn paint(h: &mut u64, p: &Paint) {
    match p {
        Paint::None => fnv(h, &[0]),
        Paint::Solid(c) => { fnv(h, &[1]); color(h, c); }
        Paint::Gradient(g) => {
            fnv(h, &[2, g.kind as u8]);
            u(h, g.stops.len() as u64);
            for st in &g.stops { f(h, st.offset); color(h, &st.color); f(h, st.bias); }
            for v in g.transform.0 { f(h, v); }
        }
    }
}
fn path(h: &mut u64, p: &Path) {
    u(h, p.subpaths.len() as u64);
    for sp in &p.subpaths {
        f(h, sp.start.x); f(h, sp.start.y);
        u(h, sp.segments.len() as u64);
        for seg in &sp.segments { for pt in seg { f(h, pt.x); f(h, pt.y); } }
        b(h, sp.closed);
    }
}
fn stroke(h: &mut u64, k: &Stroke) {
    paint(h, &k.paint);
    f(h, k.width);
    fnv(h, &[k.cap as u8, k.join as u8]);
    f(h, k.miter_limit);
    fnv(h, &[k.align as u8]);
    match &k.dash { Some((v, ph)) => { fnv(h, &[1]); u(h, v.len() as u64); for x in v { f(h, *x); } f(h, *ph); } None => fnv(h, &[0]) }
    b(h, k.behind);
}
fn image(h: &mut u64, i: &Image) {
    u(h, i.width as u64); u(h, i.height as u64);
    match &i.pixels { Pixels::Rgba8(v) => { fnv(h, &[0]); u(h, v.len() as u64); fnv(h, v); } Pixels::Encoded(v) => { fnv(h, &[1]); u(h, v.len() as u64); fnv(h, v); } }
    for v in i.transform.0 { f(h, v); }
}
fn node(h: &mut u64, n: &Node) {
    u(h, n.class.0 as u64);
    s(h, &n.name);
    b(h, n.visible); b(h, n.locked);
    f(h, n.opacity);
    fnv(h, &[n.blend as u8]);
    match &n.kind {
        Kind::Layer => fnv(h, &[0]),
        Kind::Group => fnv(h, &[1]),
        Kind::Artboard { rect, background } => {
            fnv(h, &[2]);
            for v in [rect.x0, rect.y0, rect.x1, rect.y1] { f(h, v); }
            u(h, background.len() as u64);
            for p in background { paint(h, p); }
        }
        Kind::Shape { path: p, fills, strokes, even_odd } => {
            fnv(h, &[3]);
            path(h, p);
            u(h, fills.len() as u64);
            for x in fills { paint(h, x); }
            u(h, strokes.len() as u64);
            for x in strokes { stroke(h, x); }
            b(h, *even_odd);
        }
        Kind::Text(t) => {
            fnv(h, &[4]);
            u(h, t.runs.len() as u64);
            for r in &t.runs {
                s(h, &r.text); s(h, &r.postscript); s(h, &r.family);
                u(h, r.weight as u64); b(h, r.italic); f(h, r.size); f(h, r.tracking);
                match r.leading { Some(l) => { fnv(h, &[1]); f(h, l); } None => fnv(h, &[0]) }
                paint(h, &r.fill);
            }
            fnv(h, &[t.align as u8]);
            f(h, t.anchor.x); f(h, t.anchor.y);
            match t.frame { Some(r) => { fnv(h, &[1]); for v in [r.x0, r.y0, r.x1, r.y1] { f(h, v); } } None => fnv(h, &[0]) }
            for v in t.transform.0 { f(h, v); }
        }
        Kind::Image(i) => { fnv(h, &[5]); image(h, i); }
        Kind::Unsupported => fnv(h, &[6]),
    }
    match &n.mask { Some(p) => { fnv(h, &[1]); path(h, p); } None => fnv(h, &[0]) }
    match &n.pixel_mask { Some(i) => { fnv(h, &[1]); image(h, i); } None => fnv(h, &[0]) }
    u(h, n.children.len() as u64);
    for c in &n.children { node(h, c); }
}
pub fn doc_hash(d: &Document) -> u64 {
    let mut h = H0;
    f(&mut h, d.dpi);
    u(&mut h, d.spreads.len() as u64);
    for sp in &d.spreads {
        for v in [sp.bounds.x0, sp.bounds.y0, sp.bounds.x1, sp.bounds.y1] { f(&mut h, v); }
        u(&mut h, sp.pages.len() as u64);
        for r in &sp.pages { for v in [r.x0, r.y0, r.x1, r.y1] { f(&mut h, v); } }
        b(&mut h, sp.transparent);
        u(&mut h, sp.nodes.len() as u64);
        for n in &sp.nodes { node(&mut h, n); }
    }
    s(&mut h, d.saved_by.as_deref().unwrap_or("-"));
    for w in &d.warnings { s(&mut h, w); }
    h
}

/// The three ways a damaged archive entry is reported depend on how early the decompressor notices
/// (miniz_oxide and ruzstd look further ahead than zlib and libzstd): one outcome for the sweeps.
fn entry_error(m: &str) -> &str {
    match m {
        "damaged Affinity file: corrupt compressed entry" | "damaged Affinity file: archive entry size mismatch" | "damaged Affinity file: archive entry checksum mismatch" => "damaged entry",
        _ => m,
    }
}

fn result(h: &mut u64, bytes: &[u8]) {
    let limits = Limits { max_entry: 64 << 20, max_total: 256 << 20 };
    match read(bytes, limits) { Ok(d) => u(h, doc_hash(&d)), Err(e) => s(h, entry_error(&e.to_string())) }
    match preview(bytes) { Ok(p) => { u(h, p.width as u64); u(h, p.height as u64); u(h, p.png.len() as u64); } Err(e) => s(h, &e.to_string()) }
}

fn files(dir: &std::path::Path, out: &mut Vec<std::path::PathBuf>) {
    let mut v: Vec<_> = std::fs::read_dir(dir).into_iter().flatten().flatten().map(|e| e.path()).collect();
    v.sort();
    for p in v {
        if p.is_dir() {
            files(&p, out);
        } else if p.extension().and_then(|e| e.to_str()).is_some_and(|e| ["af", "afdesign", "afphoto", "afpub", "aftemplate"].contains(&e)) {
            out.push(p);
        }
    }
}

pub fn affinity_vectors(dir: &str) {
    let root = std::path::Path::new(dir);
    let mut paths = Vec::new();
    files(root, &mut paths);
    for p in &paths {
        let name = p.strip_prefix(root).unwrap().display().to_string();
        let bytes = std::fs::read(p).unwrap();
        match read(&bytes, Limits::default()) {
            Ok(d) => {
                let nodes: usize = d.spreads.iter().map(|s| count(&s.nodes)).sum();
                println!("doc {name} {:016x} spreads {} nodes {} warnings {}", doc_hash(&d), d.spreads.len(), nodes, d.warnings.len());
            }
            Err(e) => println!("doc {name} err {e}"),
        }
        match preview(&bytes) {
            Ok(pv) => println!("preview {name} {}x{} {}", pv.width, pv.height, pv.png.len()),
            Err(e) => println!("preview {name} err {e}"),
        }
        // Truncations at 64 evenly spaced points, then 40 seeded mutations of 1..6 bytes.
        let mut h = H0;
        for k in 0..64u64 {
            let n = (bytes.len() as u64 * k / 64) as usize;
            result(&mut h, &bytes[..n]);
        }
        println!("truncate {name} {:016x}", h);
        let mut h = H0;
        let mut st = 0x5EED_u64 ^ bytes.len() as u64;
        let mut next = || { st = st.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407); st >> 33 };
        for _ in 0..40 {
            let mut c = bytes.clone();
            for _ in 0..1 + next() % 6 {
                let at = next() as usize % c.len();
                c[at] = next() as u8;
            }
            result(&mut h, &c);
        }
        println!("mutate {name} {:016x}", h);
    }
}

fn count(n: &[Node]) -> usize {
    n.iter().map(|n| 1 + count(&n.children)).sum()
}

/// Debug: each mutation's outcome for one file.
pub fn affinity_mutations(file: &str) {
    let bytes = std::fs::read(file).unwrap();
    let mut st = 0x5EED_u64 ^ bytes.len() as u64;
    let mut next = || { st = st.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407); st >> 33 };
    let limits = Limits { max_entry: 64 << 20, max_total: 256 << 20 };
    for i in 0..40 {
        let mut c = bytes.clone();
        for _ in 0..1 + next() % 6 {
            let at = next() as usize % c.len();
            c[at] = next() as u8;
        }
        let r = match read(&c, limits) { Ok(d) => format!("{:016x}", doc_hash(&d)), Err(e) => e.to_string() };
        let p = match preview(&c) { Ok(p) => format!("{}x{} {}", p.width, p.height, p.png.len()), Err(e) => e.to_string() };
        println!("{i} {r} | {p}");
    }
}
