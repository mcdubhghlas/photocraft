//! `rustref ops-vectors`: seeded random undo/redo/trim scenarios over photocraft-ops' History,
//! checked line by line by tests/ops/test_parity.c. Every scenario applies 80 operations (edits
//! of pixels, layers, groups with masks, channels and the quick mask; undo, redo, trim, budget
//! changes, purge, clear, clone, coalesced edits, transferred tiles) and hashes, after each one,
//! everything History exposes (labels, counts, returned documents and layer targets, unique and
//! pixel bytes). Lines, tab-separated:
//!
//!   step SEED I OP PAST REDO UNIQUE PIXEL HASH   every step of the first scenarios
//!   scenario SEED DEPTH STEPS HASH PAST REDO PIXEL   one line per scenario
use std::sync::Arc;

use photocraft_doc::{
    AlphaChannel, Color, ColorMode, Document, Layer, LayerId, LayerMask, PixelFormat, Rect, SampleType, Size,
    Surface,
};
use photocraft_ops::{History, LayerTarget};

const H0: u64 = 0xcbf29ce484222325;
fn fnv(h: &mut u64, b: &[u8]) {
    for &x in b {
        *h ^= x as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}
fn u64le(h: &mut u64, v: u64) {
    fnv(h, &v.to_le_bytes());
}
fn s(h: &mut u64, v: &str) {
    fnv(h, v.as_bytes());
    fnv(h, &[0]);
}
fn opt_s(h: &mut u64, v: Option<&str>) {
    match v {
        Some(v) => {
            fnv(h, &[1]);
            s(h, v);
        }
        None => fnv(h, &[0]),
    }
}

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

fn target(k: u64) -> LayerTarget {
    LayerTarget {
        active: if k % 3 == 0 { None } else { Some(LayerId(k)) },
        selected: (0..k % 3).map(|j| LayerId(k * 10 + j)).collect(),
    }
}

fn hash_target(h: &mut u64, t: &LayerTarget) {
    match t.active {
        Some(a) => {
            fnv(h, &[1]);
            u64le(h, a.0);
        }
        None => fnv(h, &[0]),
    }
    u64le(h, t.selected.len() as u64);
    for &id in &t.selected {
        u64le(h, id.0);
    }
}

/// The current document's observable state: name, structure, and the pixels at every point an
/// edit touched (layer 0, the top layer, the first channel).
fn hash_doc(h: &mut u64, d: &Document, points: &[(i32, i32)]) {
    s(h, &d.name);
    u64le(h, d.layers.len() as u64);
    u64le(h, d.walk().len() as u64);
    u64le(h, d.channels.len() as u64);
    fnv(h, &[d.quick_mask.is_some() as u8]);
    let mut px = [0f32; 8];
    let surfaces = [
        d.layers.first().and_then(|l| l.surface()),
        d.layers.last().and_then(|l| l.surface()),
        d.channels.first().map(|c| &c.surface),
    ];
    for surface in surfaces {
        match surface {
            Some(sf) => {
                for &(x, y) in points {
                    let n = sf.format().channels();
                    sf.read_pixel(x, y, &mut px[..n]);
                    for v in &px[..n] {
                        fnv(h, &v.to_bits().to_le_bytes());
                    }
                }
            }
            None => fnv(h, &[0xFF]),
        }
    }
}

fn hash_history(h: &mut u64, hist: &History, cur: &Document) -> (usize, usize) {
    u64le(h, hist.past_len() as u64);
    let entries = hist.entries();
    u64le(h, entries.len() as u64);
    for e in &entries {
        s(h, e);
    }
    let redo: Vec<&str> = hist.redo_labels().collect();
    u64le(h, redo.len() as u64);
    for r in &redo {
        s(h, r);
    }
    opt_s(h, hist.undo_label());
    opt_s(h, hist.redo_label());
    fnv(h, &[hist.can_undo() as u8, hist.can_redo() as u8]);
    let unique = hist.unique_bytes(cur);
    let pixel = hist.pixel_bytes(cur);
    u64le(h, unique as u64);
    u64le(h, pixel as u64);
    for i in 0..=hist.past_len() {
        match hist.state(i) {
            Some(d) => s(h, &d.name),
            None => fnv(h, &[0xFE]),
        }
    }
    (unique, pixel)
}

const BUDGETS: [usize; 8] = [0, 1, 65536, 262144, 524288, 1048576, 4194304, usize::MAX];

fn paint_layer(d: &mut Document, idx: usize, x: i32, y: i32, v: f32) {
    if let Some(sf) = d.layers.get_mut(idx).and_then(|l| l.surface_mut()) {
        let n = sf.format().channels();
        let px = [v, 0.5, 0.25, 1.0, 0.75];
        sf.write_pixel(x, y, &px[..n.min(5)]);
    }
}

fn scenario(seed: u64, steps: usize, verbose: bool) {
    let depths = [SampleType::U8, SampleType::U16, SampleType::F32];
    let depth = depths[(seed % 3) as usize];
    let w = 64 + (seed * 37 % 600) as u32;
    let hgt = 64 + (seed * 53 % 400) as u32;
    let mut rng = Rng(seed.wrapping_mul(0x9E3779B97F4A7C15) | 1);
    let doc = Document::with_background("h", Size::new(w, hgt), ColorMode::Rgb, depth, Color::WHITE);
    let mut cur = Arc::new(doc);
    let max_states = if seed % 5 == 0 { 50 } else { 1 + (seed % 7) as usize };
    let mut hist = History::new(max_states);
    let mut points: Vec<(i32, i32)> = vec![(1, 1)];
    let mut h = H0;
    let mut last = (0usize, 0usize);
    for i in 0..steps {
        let op = rng.below(20);
        let k = i as u64 + 1;
        let x = rng.below(w as u64 + 40) as i32 - 20;
        let y = rng.below(hgt as u64 + 40) as i32 - 20;
        let v = (k % 11) as f32 / 10.0;
        let mut sh = H0;
        u64le(&mut sh, op);
        let record = |hist: &mut History, cur: &mut Arc<Document>, label: String,
                      f: &mut dyn FnMut(&mut Document)| {
            let before = cur.clone();
            let mut d = (**cur).clone();
            f(&mut d);
            *cur = Arc::new(d);
            hist.record(label, before, target(k));
        };
        match op {
            0..=3 => {
                points.push((x, y));
                let n = cur.layers.len() as u64;
                let idx = rng.below(n.max(1)) as usize;
                record(&mut hist, &mut cur, format!("Paint {k}"), &mut |d| paint_layer(d, idx, x, y, v));
            }
            4 => record(&mut hist, &mut cur, "Rename".into(), &mut |d| d.name = format!("n{k}")),
            5 => {
                points.push((x, y));
                record(&mut hist, &mut cur, "New Layer".into(), &mut |d| {
                    let mut l = Layer::raster("L", d.pixel_format());
                    if let Some(sf) = l.surface_mut() {
                        sf.write_pixel(x, y, &[v, v, v, 1.0]);
                    }
                    d.insert_above(None, l);
                })
            }
            6 => {
                points.push((x, y));
                record(&mut hist, &mut cur, "New Group".into(), &mut |d| {
                    let mut child = Layer::raster("C", d.pixel_format());
                    if let Some(sf) = child.surface_mut() {
                        sf.write_pixel(x, y, &[0.0, v, 0.0, 1.0]);
                    }
                    let mut mask = LayerMask::reveal_all();
                    mask.surface.write_pixel(x, y, &[v]);
                    child.mask = Some(mask);
                    let inner = Layer::group("Inner", vec![child]);
                    d.insert_above(None, Layer::group("G", vec![inner]));
                })
            }
            7 => {
                points.push((x, y));
                record(&mut hist, &mut cur, "New Channel".into(), &mut |d| {
                    let mut g = Surface::new(PixelFormat::GRAY8);
                    g.write_pixel(x, y, &[v]);
                    d.channels.push(AlphaChannel::new("Alpha", g));
                })
            }
            8 => record(&mut hist, &mut cur, "Quick Mask".into(), &mut |d| {
                if d.quick_mask.is_some() {
                    d.quick_mask = None;
                } else {
                    let g = match d.channels.first() {
                        Some(c) => c.surface.clone(),
                        None => Surface::new(PixelFormat::GRAY8),
                    };
                    d.quick_mask = Some(AlphaChannel::new("Quick Mask", g));
                }
            }),
            9 | 10 | 11 => {
                let r = if op == 11 { hist.redo(cur.clone()) } else { hist.undo(cur.clone()) };
                match r {
                    Some((d, layers)) => {
                        fnv(&mut sh, &[1]);
                        hash_target(&mut sh, &layers);
                        cur = d;
                    }
                    None => fnv(&mut sh, &[0]),
                }
            }
            12 => {
                let dropped = hist.trim(&cur);
                u64le(&mut sh, dropped as u64);
            }
            13 => hist.max_bytes = BUDGETS[rng.below(BUDGETS.len() as u64) as usize],
            14 => fnv(&mut sh, &[hist.purge_last() as u8]),
            15 => hist.clear_redo(),
            16 => hist.record("Same", cur.clone(), target(k)),
            17 => {
                // A coalesced edit changes current without recording; sometimes by moving a
                // tile of another format (and byte length) in.
                points.push((x, y));
                let d = Arc::make_mut(&mut cur);
                if k % 2 == 0 {
                    paint_layer(d, 0, x, y, v);
                } else if let Some(sf) = d.layers.first_mut().and_then(|l| l.surface_mut()) {
                    let mut large = Surface::new(PixelFormat::new(ColorMode::Cmyk, SampleType::F32, true));
                    large.write_pixel(x, y, &[v, 0.0, 0.0, 0.0, 1.0]);
                    sf.put_tiles(large.take_tiles(Rect::from_xywh(x, y, 1, 1)));
                }
            }
            18 => {
                if k % 2 == 0 {
                    hist.set_current_label(format!("Fold {k}"));
                } else {
                    hist.set_current_layers(target(k));
                }
            }
            _ => {
                if rng.below(4) == 0 {
                    hist.clear();
                } else {
                    hist = hist.clone();
                }
            }
        }
        last = hash_history(&mut sh, &hist, &cur);
        hash_doc(&mut sh, &cur, &points);
        u64le(&mut h, sh);
        if verbose {
            let (past, redo) = (hist.past_len(), hist.redo_labels().count());
            println!("step\t{seed}\t{i}\t{op}\t{past}\t{redo}\t{}\t{}\t{sh:016x}", last.0, last.1);
        }
    }
    let _ = last;
    println!(
        "scenario\t{seed}\t{depth:?}\t{steps}\t{h:016x}\t{}\t{}\t{}",
        hist.past_len(),
        hist.redo_labels().count(),
        hist.pixel_bytes(&cur)
    );
}

pub fn run() {
    for seed in 1..=120u64 {
        scenario(seed, 80, seed <= 4);
    }
}
