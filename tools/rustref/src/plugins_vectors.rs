//! I6 plug-ins reference vectors (`rustref plugins-vectors`), checked by tests/plugins/test_parity.c.
//! Lines are tab-separated; byte strings (inputs, modules) are lower-case hex; result texts escape
//! `\` `\t` `\n` as `\\` `\t` `\n`.
//!
//!   man HEX RESULT                   Manifest::parse(bytes): `ok NOTATION JSON` (serde_json::to_string)
//!                                    or `err MESSAGE` (the Error's Display text)
//!   resman M HEX                     the manifests resolve_params runs against
//!   res M HEX RESULT                 MANIFESTS[M].resolve_params(json): `ok JSON` or `err MESSAGE`
//!   mod NAME HEX                     a WebAssembly module (built here with a tiny encoder; the C test
//!                                    also rebuilds the named ABI modules and compares bytes)
//!   load NAME LIMITS RESULT          Plugin::load: `ok ID NAME NOTATION SIZE` or `err KIND MESSAGE`
//!   apply NAME LIMITS CASE RESULT    Plugin::apply: `ok TILES BOUNDS HASH` (BLAKE3 of the f32 LE samples
//!                                    of read_region(CANVAS)) or `err KIND MESSAGE`; `panic` if it panicked
//!   area NAME CASE X0 Y0 X1 Y1       Plugin::output_area
//!   fmt MODE SAMPLE ALPHA CODE       format_code
//!   reg STEP RESULT                  registry operations in order
//!   folder STEP RESULT               registry::load_folder / load_file over a scratch directory
//!
//! KIND is the Error variant: Module Abi Manifest Trap Limit Params Failed NotFound Io.
use std::panic::{catch_unwind, AssertUnwindSafe};

use photocraft_color::{ColorMode, PixelFormat, SampleType};
use photocraft_geom::Rect;
use photocraft_plugins::{format_code, registry, Error, Limits, Manifest, Plugin};
use photocraft_raster::Surface;
use serde_json::{json, Value};

const INVERT: &[u8] = include_bytes!("../../../crates/plugins/tests/fixtures/invert.wasm");

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn esc(s: &str) -> String {
    s.replace('\\', "\\\\").replace('\t', "\\t").replace('\n', "\\n")
}

fn kind(e: &Error) -> &'static str {
    match e {
        Error::Module(_) => "Module",
        Error::Abi(_) => "Abi",
        Error::Manifest(_) => "Manifest",
        Error::Trap(_) => "Trap",
        Error::Limit(_) => "Limit",
        Error::Params(_) => "Params",
        Error::Failed(_) => "Failed",
        Error::NotFound(_) => "NotFound",
        Error::Io(_) => "Io",
    }
}

fn err_text(e: &Error) -> String {
    format!("err\t{}\t{}", kind(e), esc(&e.to_string()))
}

// ---------------------------------------------------------------------------
// A tiny WebAssembly encoder (the `wat` crate is not available offline)
// ---------------------------------------------------------------------------

const I32: u8 = 0x7f;
const I64: u8 = 0x7e;

fn uleb(o: &mut Vec<u8>, mut v: u64) {
    loop {
        let b = (v & 0x7f) as u8;
        v >>= 7;
        if v == 0 {
            o.push(b);
            return;
        }
        o.push(b | 0x80);
    }
}

fn sleb(o: &mut Vec<u8>, mut v: i64) {
    loop {
        let b = (v & 0x7f) as u8;
        v >>= 7;
        let done = (v == 0 && b & 0x40 == 0) || (v == -1 && b & 0x40 != 0);
        o.push(if done { b } else { b | 0x80 });
        if done {
            return;
        }
    }
}

fn name(o: &mut Vec<u8>, s: &[u8]) {
    uleb(o, s.len() as u64);
    o.extend_from_slice(s);
}

#[derive(Default, Clone)]
struct M {
    types: Vec<(Vec<u8>, Vec<u8>)>,
    imports: Vec<(Vec<u8>, Vec<u8>, Vec<u8>)>, // module, name, kind byte + description
    funcs: Vec<(u32, Vec<(u32, u8)>, Vec<u8>)>,
    tables: Vec<Vec<u8>>,  // raw table type
    memories: Vec<Vec<u8>>, // raw memory type
    globals: Vec<Vec<u8>>,  // raw global (type + init)
    exports: Vec<(Vec<u8>, u8, u32)>,
    start: Option<u32>,
    elems: Vec<Vec<u8>>, // raw element segments
    data: Vec<Vec<u8>>,  // raw data segments
    raw_code: Option<Vec<u8>>, // replaces the code section payload
    extra_sections: Vec<(usize, u8, Vec<u8>)>, // (insert before section index, id, payload)
}

impl M {
    fn ty(&mut self, p: &[u8], r: &[u8]) -> u32 {
        if let Some(i) = self.types.iter().position(|t| t.0 == p && t.1 == r) {
            return i as u32;
        }
        self.types.push((p.to_vec(), r.to_vec()));
        (self.types.len() - 1) as u32
    }
    fn func(&mut self, p: &[u8], r: &[u8], locals: &[(u32, u8)], body: &[u8]) -> u32 {
        let t = self.ty(p, r);
        self.funcs.push((t, locals.to_vec(), body.to_vec()));
        (self.imports.iter().filter(|i| i.2[0] == 0).count() + self.funcs.len() - 1) as u32
    }
    fn export(&mut self, n: &str, kind: u8, idx: u32) {
        self.exports.push((n.as_bytes().to_vec(), kind, idx));
    }
    fn encode(&self) -> Vec<u8> {
        let mut out = b"\0asm\x01\0\0\0".to_vec();
        let mut sections: Vec<(u8, Vec<u8>)> = Vec::new();
        let vec_sec = |items: &[Vec<u8>]| {
            let mut p = Vec::new();
            uleb(&mut p, items.len() as u64);
            for i in items {
                p.extend_from_slice(i);
            }
            p
        };
        if !self.types.is_empty() {
            let items: Vec<Vec<u8>> = self
                .types
                .iter()
                .map(|(p, r)| {
                    let mut e = vec![0x60];
                    uleb(&mut e, p.len() as u64);
                    e.extend_from_slice(p);
                    uleb(&mut e, r.len() as u64);
                    e.extend_from_slice(r);
                    e
                })
                .collect();
            sections.push((1, vec_sec(&items)));
        }
        if !self.imports.is_empty() {
            let items: Vec<Vec<u8>> = self
                .imports
                .iter()
                .map(|(m, n, d)| {
                    let mut e = Vec::new();
                    name(&mut e, m);
                    name(&mut e, n);
                    e.extend_from_slice(d);
                    e
                })
                .collect();
            sections.push((2, vec_sec(&items)));
        }
        if !self.funcs.is_empty() {
            let items: Vec<Vec<u8>> = self
                .funcs
                .iter()
                .map(|f| {
                    let mut e = Vec::new();
                    uleb(&mut e, f.0 as u64);
                    e
                })
                .collect();
            sections.push((3, vec_sec(&items)));
        }
        if !self.tables.is_empty() {
            sections.push((4, vec_sec(&self.tables)));
        }
        if !self.memories.is_empty() {
            sections.push((5, vec_sec(&self.memories)));
        }
        if !self.globals.is_empty() {
            sections.push((6, vec_sec(&self.globals)));
        }
        if !self.exports.is_empty() {
            let items: Vec<Vec<u8>> = self
                .exports
                .iter()
                .map(|(n, k, i)| {
                    let mut e = Vec::new();
                    name(&mut e, n);
                    e.push(*k);
                    uleb(&mut e, *i as u64);
                    e
                })
                .collect();
            sections.push((7, vec_sec(&items)));
        }
        if let Some(s) = self.start {
            let mut p = Vec::new();
            uleb(&mut p, s as u64);
            sections.push((8, p));
        }
        if !self.elems.is_empty() {
            sections.push((9, vec_sec(&self.elems)));
        }
        if let Some(raw) = &self.raw_code {
            sections.push((10, raw.clone()));
        } else if !self.funcs.is_empty() {
            let items: Vec<Vec<u8>> = self
                .funcs
                .iter()
                .map(|(_, locals, body)| {
                    let mut f = Vec::new();
                    uleb(&mut f, locals.len() as u64);
                    for (n, t) in locals {
                        uleb(&mut f, *n as u64);
                        f.push(*t);
                    }
                    f.extend_from_slice(body);
                    let mut e = Vec::new();
                    uleb(&mut e, f.len() as u64);
                    e.extend_from_slice(&f);
                    e
                })
                .collect();
            sections.push((10, vec_sec(&items)));
        }
        if !self.data.is_empty() {
            sections.push((11, vec_sec(&self.data)));
        }
        for (at, id, p) in self.extra_sections.iter().rev() {
            sections.insert((*at).min(sections.len()), (*id, p.clone()));
        }
        for (id, p) in sections {
            out.push(id);
            uleb(&mut out, p.len() as u64);
            out.extend_from_slice(&p);
        }
        out
    }
}

fn mem(min: u64, max: Option<u64>) -> Vec<u8> {
    let mut e = vec![if max.is_some() { 1 } else { 0 }];
    uleb(&mut e, min);
    if let Some(m) = max {
        uleb(&mut e, m);
    }
    e
}

fn data_seg(offset: i32, bytes: &[u8]) -> Vec<u8> {
    let mut e = vec![0, 0x41];
    sleb(&mut e, offset as i64);
    e.push(0x0b);
    name(&mut e, bytes);
    e
}

const MANIFEST: &str = r#"{"id":"test.plugin","name":"Test","version":"1","kind":"filter"}"#;

/// pc_alloc: grows memory by ceil(n / 64 KiB) pages and returns the old size (0 on failure).
const ALLOC_BODY: &[u8] = &[
    0x20, 0x00, 0x41, 0xff, 0xff, 0x03, 0x6a, 0x41, 0x10, 0x76, 0x40, 0x00, 0x22, 0x01, 0x41, 0x7f, 0x46, 0x04, 0x7f, 0x41,
    0x00, 0x05, 0x20, 0x01, 0x41, 0x80, 0x80, 0x04, 0x6c, 0x0b, 0x0b,
];

/// The ABI module of crates/plugins/tests/abi.rs: `manifest` at offset 16, `version`, the
/// pc_filter body (params $buf=0 $len=1 $w=2 $h=3 $ch=4 $fmt=5 $pp=6 $pl=7, locals from 8).
fn abi(manifest: &str, version: i32, filter_locals: &[(u32, u8)], filter: &[u8]) -> M {
    let mut m = M::default();
    m.memories.push(mem(1, None));
    m.data.push(data_seg(16, manifest.as_bytes()));
    let mut v = vec![0x41];
    sleb(&mut v, version as i64);
    v.push(0x0b);
    let fv = m.func(&[], &[I32], &[], &v);
    let mut mb = vec![0x42];
    sleb(&mut mb, ((manifest.len() as i64) << 32) | 16);
    mb.push(0x0b);
    let fm = m.func(&[], &[I64], &[], &mb);
    let fa = m.func(&[I32], &[I32], &[(1, I32)], ALLOC_BODY);
    let ff = m.func(&[I32; 8], &[I32], filter_locals, filter);
    m.export("memory", 2, 0);
    m.export("pc_abi_version", 0, fv);
    m.export("pc_manifest", 0, fm);
    m.export("pc_alloc", 0, fa);
    m.export("pc_filter", 0, ff);
    m
}

fn f32c(v: f32) -> Vec<u8> {
    let mut b = vec![0x43];
    b.extend_from_slice(&v.to_le_bytes());
    b
}

/// for (i = 0; i < len; i += 4) f32.store(buf + i, VALUE); return 0 — VALUE is an expression.
fn fill_body(value: &[u8]) -> Vec<u8> {
    let mut b = vec![0x02, 0x40, 0x03, 0x40, 0x20, 0x08, 0x20, 0x01, 0x4f, 0x0d, 0x01, 0x20, 0x00, 0x20, 0x08, 0x6a];
    b.extend_from_slice(value);
    b.extend_from_slice(&[0x38, 0x02, 0x00, 0x20, 0x08, 0x41, 0x04, 0x6a, 0x21, 0x08, 0x0c, 0x00, 0x0b, 0x0b, 0x41, 0x00, 0x0b]);
    b
}

const SPIN: &[u8] = &[0x03, 0x40, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b];

/// Every named module, in a fixed order.
fn modules() -> Vec<(&'static str, Vec<u8>)> {
    let mut v: Vec<(&'static str, Vec<u8>)> = Vec::new();
    v.push(("invert", INVERT.to_vec()));
    let ok = |body: &[u8]| abi(MANIFEST, 1, &[], body).encode();
    v.push(("noop", ok(&[0x41, 0x00, 0x0b])));
    // params_len / 1000 into every sample
    let mut val = vec![0x20, 0x07, 0xb3];
    val.extend(f32c(1000.0));
    val.push(0x95);
    let man = r#"{"id":"test.params","name":"P","kind":"filter","params":{"amount":{"type":"number","min":0,"max":10,"default":5}}}"#;
    v.push(("params", abi(man, 1, &[(1, I32)], &fill_body(&val)).encode()));
    v.push(("nan", abi(MANIFEST, 1, &[(1, I32)], &fill_body(&f32c(f32::NAN))).encode()));
    // sample i gets 2.0, -1.0, +inf, -0.5 or 0.25 by (i / 4) % 5
    let mut sel = Vec::new();
    // if (i>>2)%5==0 {2.0} else if ==1 {-1.0} else if ==2 {inf} else if ==3 {-0.5} else {0.25}
    let consts = [2.0f32, -1.0, f32::INFINITY, -0.5];
    for (k, c) in consts.iter().enumerate() {
        sel.extend_from_slice(&[0x20, 0x08, 0x41, 0x02, 0x76, 0x41, 0x05, 0x70, 0x41, k as u8, 0x46, 0x04, 0x7d]);
        sel.extend(f32c(*c));
        sel.push(0x05);
    }
    sel.extend(f32c(0.25));
    sel.extend_from_slice(&[0x0b, 0x0b, 0x0b, 0x0b]);
    v.push(("sanitize", abi(MANIFEST, 1, &[(1, I32)], &fill_body(&sel)).encode()));
    // buf[0..4] = w, h, channels, format (as f32)
    let mut args = Vec::new();
    for (k, p) in [2u8, 3, 4, 5].iter().enumerate() {
        args.extend_from_slice(&[0x20, 0x00, 0x20, *p, 0xb3, 0x38, 0x02]);
        uleb(&mut args, (k * 4) as u64);
    }
    args.extend_from_slice(&[0x41, 0x00, 0x0b]);
    v.push(("args", ok(&args)));
    // overlap 3: every sample takes the value of the same channel 3 pixels to the left
    // for (i = len - 4; i >= ch*12; i -= 4) buf[i] = buf[i - ch*12]
    let man = r#"{"id":"test.shift","name":"Shift","kind":"filter","overlap":3}"#;
    let shift = vec![
        0x20, 0x01, 0x41, 0x04, 0x6b, 0x21, 0x08, // i = len - 4
        0x20, 0x04, 0x41, 0x0c, 0x6c, 0x21, 0x09, // d = ch * 12
        0x02, 0x40, 0x03, 0x40, //
        0x20, 0x08, 0x20, 0x09, 0x48, 0x0d, 0x01, // if (i < d) break (signed)
        0x20, 0x00, 0x20, 0x08, 0x6a, // buf + i
        0x20, 0x00, 0x20, 0x08, 0x6a, 0x20, 0x09, 0x6b, 0x2a, 0x02, 0x00, // f32.load(buf + i - d)
        0x38, 0x02, 0x00, // store
        0x20, 0x08, 0x41, 0x04, 0x6b, 0x21, 0x08, 0x0c, 0x00, 0x0b, 0x0b, 0x41, 0x00, 0x0b,
    ];
    v.push(("shift", abi(man, 1, &[(2, I32)], &shift).encode()));
    // area "canvas": fills every sample with 0.5
    let man = r#"{"id":"test.clouds","name":"Clouds","kind":"filter","area":"canvas"}"#;
    v.push(("canvas", abi(man, 1, &[(1, I32)], &fill_body(&f32c(0.5))).encode()));
    // bulk memory: memory.fill(buf, 0, len)
    v.push(("fill0", ok(&[0x20, 0x00, 0x41, 0x00, 0x20, 0x01, 0xfc, 0x0b, 0x00, 0x41, 0x00, 0x0b])));
    // hostile pc_filter bodies (abi.rs hostile_modules_fail_quickly)
    v.push(("spin", ok(SPIN)));
    v.push((
        "grow-bomb",
        ok(&[0x03, 0x40, 0x41, 0x80, 0x04, 0x40, 0x00, 0x41, 0x7f, 0x47, 0x0d, 0x00, 0x0b, 0x00, 0x0b]),
    ));
    v.push(("grow-forever", ok(&[0x03, 0x40, 0x41, 0x01, 0x40, 0x00, 0x1a, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b])));
    v.push(("oob-write", ok(&[0x41, 0x7c, 0x41, 0x00, 0x36, 0x02, 0x00, 0x41, 0x00, 0x0b])));
    v.push(("trap", ok(&[0x00, 0x0b])));
    v.push(("div-zero", ok(&[0x41, 0x01, 0x20, 0x04, 0x6e, 0x1a, 0x41, 0x01, 0x41, 0x00, 0x6e, 0x0b])));
    v.push(("int-overflow", ok(&[0x41, 0x80, 0x80, 0x80, 0x80, 0x78, 0x41, 0x7f, 0x6d, 0x0b])));
    let mut conv = f32c(f32::NAN);
    conv.extend_from_slice(&[0xa8, 0x0b]);
    v.push(("bad-conversion", ok(&conv)));
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x10, 0x04, 0x41, 0x00, 0x0b]);
        m.func(&[], &[], &[], &[0x10, 0x04, 0x0b]); // $r: call $r (index 4)
        v.push(("recursion", m.encode()));
    }
    v.push(("error-code", ok(&[0x41, 0x07, 0x0b])));
    v.push(("abi-v2", abi(MANIFEST, 2, &[], &[0x41, 0x00, 0x0b]).encode()));
    v.push(("bad-manifest", abi("not json", 1, &[], &[0x41, 0x00, 0x0b]).encode()));
    v.push(("bad-id", abi(r#"{"id":"a b","name":"x","kind":"filter"}"#, 1, &[], &[0x41, 0x00, 0x0b]).encode()));
    v.push(("wrong-kind", abi(r#"{"id":"a","name":"x","kind":"panel"}"#, 1, &[], &[0x41, 0x00, 0x0b]).encode()));
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        let s = m.func(&[], &[], &[], &[0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b]);
        m.start = Some(s);
        v.push(("start-loops", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        let s = m.func(&[], &[], &[], &[0x00, 0x0b]);
        m.start = Some(s);
        v.push(("start-traps", m.encode()));
    }
    {
        // a start function that writes a marker the filter then reports
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        let s = m.func(&[], &[], &[], &[0x41, 0x08, 0x41, 0x2a, 0x36, 0x02, 0x00, 0x0b]);
        m.start = Some(s);
        v.push(("start-ok", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        let s = m.func(&[I32], &[], &[], &[0x0b]);
        m.start = Some(s);
        v.push(("start-bad-type", m.encode()));
    }
    {
        // WAMR runs these exports by itself; the reference never does
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        let c = m.func(&[], &[], &[], &[0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b]);
        m.export("__wasm_call_ctors", 0, c);
        m.export("__post_instantiate", 0, c);
        v.push(("ctors-loop", m.encode()));
    }
    let import = |module: &str, field: &str, desc: &[u8]| {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        // imported functions shift the function index space: rebuild the exports
        m.imports.push((module.as_bytes().to_vec(), field.as_bytes().to_vec(), desc.to_vec()));
        if desc[0] == 0 {
            for e in m.exports.iter_mut().filter(|e| e.1 == 0) {
                e.2 += 1;
            }
        }
        m
    };
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        let t = m.ty(&[I32; 4], &[I32]);
        let mut d = vec![0];
        uleb(&mut d, t as u64);
        let mut m2 = import("wasi_snapshot_preview1", "fd_write", &d);
        m2.types = m.types.clone();
        v.push(("wasi-import", m2.encode()));
    }
    v.push(("memory-import", {
        let mut m = import("env", "memory", &[2, 0, 1]);
        m.memories.clear();
        m.encode()
    }));
    v.push(("global-import", import("env", "g", &[3, I32, 0]).encode()));
    v.push(("table-import", import("env", "t", &[1, 0x70, 0, 1]).encode()));
    {
        let mut m = M::default();
        m.memories.push(mem(60000, None));
        m.export("memory", 2, 0);
        v.push(("huge-memory", m.encode()));
    }
    {
        let mut m = M::default();
        m.memories.push(mem(1, None));
        m.export("memory", 2, 0);
        let a = m.func(&[], &[I32], &[], &[0x41, 0x01, 0x0b]);
        let b = m.func(&[], &[I64], &[], &[0x42, 0x00, 0x0b]);
        m.export("pc_abi_version", 0, a);
        m.export("pc_manifest", 0, b);
        v.push(("no-filter", m.encode()));
    }
    let custom = |manifest_ret: i64, alloc: &[u8]| {
        let mut m = M::default();
        m.memories.push(mem(1, None));
        m.data.push(data_seg(16, MANIFEST.as_bytes()));
        m.export("memory", 2, 0);
        let a = m.func(&[], &[I32], &[], &[0x41, 0x01, 0x0b]);
        let mut mb = vec![0x42];
        sleb(&mut mb, manifest_ret);
        mb.push(0x0b);
        let b = m.func(&[], &[I64], &[], &mb);
        let c = m.func(&[I32], &[I32], &[], alloc);
        let d = m.func(&[I32; 8], &[I32], &[], &[0x41, 0x00, 0x0b]);
        m.export("pc_abi_version", 0, a);
        m.export("pc_manifest", 0, b);
        m.export("pc_alloc", 0, c);
        m.export("pc_filter", 0, d);
        m
    };
    let packed = ((MANIFEST.len() as i64) << 32) | 16;
    v.push(("manifest-outside", custom(0x0000_0064_ffff_ff00, &[0x41, 0x10, 0x0b]).encode()));
    v.push(("bogus-alloc", custom(packed, &[0x41, 0x70, 0x0b]).encode()));
    v.push(("alloc-zero", custom(packed, &[0x41, 0x00, 0x0b]).encode()));
    v.push(("alloc-traps", custom(packed, &[0x00, 0x0b]).encode()));
    v.push(("manifest-huge", custom((70_000i64 << 32) | 16, &[0x41, 0x10, 0x0b]).encode()));
    {
        let mut m = custom(((4i64) << 32) | 0x400, &[0x41, 0x10, 0x0b]);
        m.data.push(data_seg(0x400, &[0xff, 0xfe, 0x41, 0x42]));
        v.push(("manifest-not-utf8", m.encode()));
    }
    {
        let mut m = custom(packed, &[0x41, 0x10, 0x0b]);
        m.exports.retain(|e| e.0 != b"memory");
        v.push(("no-memory-export", m.encode()));
    }
    {
        let mut m = custom(packed, &[0x41, 0x10, 0x0b]);
        m.exports.retain(|e| e.0 != b"pc_alloc");
        v.push(("no-alloc", m.encode()));
    }
    {
        let mut m = custom(packed, &[0x41, 0x10, 0x0b]);
        let f = m.func(&[I64], &[I32], &[], &[0x41, 0x10, 0x0b]);
        for e in m.exports.iter_mut().filter(|e| e.0 == b"pc_alloc") {
            e.2 = f;
        }
        v.push(("alloc-i64", m.encode()));
    }
    {
        let mut m = custom(packed, &[0x41, 0x10, 0x0b]);
        let f = m.func(&[I32; 7], &[I32], &[], &[0x41, 0x00, 0x0b]);
        for e in m.exports.iter_mut().filter(|e| e.0 == b"pc_filter") {
            e.2 = f;
        }
        v.push(("filter-7", m.encode()));
    }
    {
        let mut m = custom(packed, &[0x41, 0x10, 0x0b]);
        let f = m.func(&[], &[I64], &[], &[0x42, 0x01, 0x0b]);
        for e in m.exports.iter_mut().filter(|e| e.0 == b"pc_abi_version") {
            e.2 = f;
        }
        v.push(("version-i64", m.encode()));
    }
    {
        let mut m = custom(packed, &[0x41, 0x10, 0x0b]);
        let f = m.func(&[], &[I32], &[], &[0x41, 0x10, 0x0b]);
        for e in m.exports.iter_mut().filter(|e| e.0 == b"pc_manifest") {
            e.2 = f;
        }
        v.push(("manifest-i32", m.encode()));
    }
    {
        let mut m = custom(packed, &[0x41, 0x10, 0x0b]);
        for e in m.exports.iter_mut().filter(|e| e.0 == b"pc_alloc") {
            e.1 = 3; // a global export named pc_alloc
            e.2 = 0;
        }
        m.globals.push(vec![I32, 0, 0x41, 0x00, 0x0b]);
        v.push(("alloc-is-global", m.encode()));
    }
    {
        let mut m = custom(packed, &[0x00, 0x0b]);
        for e in m.exports.iter_mut().filter(|e| e.0 == b"pc_abi_version") {
            e.2 = 2; // pc_alloc's index: traps
        }
        // pc_abi_version must be () -> i32: point it at a trapping one
        let f = m.func(&[], &[I32], &[], &[0x00, 0x0b]);
        for e in m.exports.iter_mut().filter(|e| e.0 == b"pc_abi_version") {
            e.2 = f;
        }
        v.push(("version-traps", m.encode()));
    }
    {
        let mut m = custom(packed, &[0x41, 0x10, 0x0b]);
        let f = m.func(&[], &[I32], &[], SPIN);
        for e in m.exports.iter_mut().filter(|e| e.0 == b"pc_abi_version") {
            e.2 = f;
        }
        v.push(("version-loops", m.encode()));
    }
    {
        // data segment past the end of memory: instantiation fails
        let mut m = custom(packed, &[0x41, 0x10, 0x0b]);
        m.data.push(data_seg(65530, b"0123456789"));
        v.push(("data-oob", m.encode()));
    }
    {
        // tail call: pc_abi_version = return_call $one
        let mut m = custom(packed, &[0x41, 0x10, 0x0b]);
        let one = m.func(&[], &[I32], &[], &[0x41, 0x01, 0x0b]);
        let mut b = vec![0x12];
        uleb(&mut b, one as u64);
        b.push(0x0b);
        let f = m.func(&[], &[I32], &[], &b);
        for e in m.exports.iter_mut().filter(|e| e.0 == b"pc_abi_version") {
            e.2 = f;
        }
        v.push(("tail-call", m.encode()));
    }
    // reference types: call_indirect through a table
    let indirect = |idx: i32, elem: bool, wrong_type: bool| {
        let mut m = abi(MANIFEST, 1, &[], &[]);
        let t0 = m.ty(&[], &[I32]);
        let mut body = vec![0x41];
        sleb(&mut body, idx as i64);
        body.push(0x11);
        uleb(&mut body, t0 as u64);
        body.extend_from_slice(&[0x00, 0x1a, 0x41, 0x00, 0x0b]);
        m.funcs[3].2 = body;
        m.tables.push(vec![0x70, 0x00, 0x02]);
        if elem {
            let target = if wrong_type { 2 } else { 0 }; // pc_alloc (i32)->i32, or pc_abi_version
            m.elems.push(vec![0x00, 0x41, 0x00, 0x0b, 0x01, target]);
        }
        m.encode()
    };
    v.push(("indirect-null", indirect(1, true, false)));
    v.push(("indirect-oob", indirect(5, true, false)));
    v.push(("indirect-mismatch", indirect(0, true, true)));
    v.push(("indirect-ok", indirect(0, true, false)));
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        for _ in 0..5 {
            m.tables.push(vec![0x70, 0x00, 0x01]);
        }
        v.push(("five-tables", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        let mut t = vec![0x70, 0x00];
        uleb(&mut t, 200_000);
        m.tables.push(t);
        v.push(("big-table", m.encode()));
    }
    // wasmi EnforcedLimits::strict
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        m.ty(&[I32; 33], &[]);
        v.push(("limit-params", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        m.ty(&[], &[I32; 33]);
        v.push(("limit-results", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        m.memories.push(mem(1, None));
        v.push(("limit-memories", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        for _ in 0..101 {
            m.tables.push(vec![0x70, 0x00, 0x00]);
        }
        v.push(("limit-tables", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        for _ in 0..1001 {
            m.globals.push(vec![I32, 0, 0x41, 0x00, 0x0b]);
        }
        v.push(("limit-globals", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        for _ in 0..10_001 {
            m.func(&[], &[], &[], &[0x0b]);
        }
        v.push(("limit-functions", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        for _ in 0..40 {
            m.func(&[], &[], &[], &[0x01; 30].iter().copied().chain([0x0b]).collect::<Vec<u8>>());
        }
        v.push(("limit-avg-bytes", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        for _ in 0..1001 {
            m.data.push(data_seg(0, b""));
        }
        v.push(("limit-data", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        for _ in 0..1001 {
            m.elems.push(vec![0x01, 0x00, 0x00]); // passive, funcref, no items
        }
        v.push(("limit-elems", m.encode()));
    }
    {
        // a code section claiming 0 bodies followed by 1200 bytes (wasmi divides by the count)
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        let mut raw = vec![0x00];
        raw.extend(std::iter::repeat_n(0u8, 1200));
        m.raw_code = Some(raw);
        v.push(("code-count-zero", m.encode()));
    }
    // malformed binaries
    v.push(("empty", Vec::new()));
    v.push(("garbage", b"\0asm\x01\0\0\0\xff\xff\xff".to_vec()));
    v.push(("not-wasm", b"MZ\x90\0".to_vec()));
    v.push(("truncated", INVERT[..INVERT.len() / 2].to_vec()));
    v.push(("version-2", b"\0asm\x02\0\0\0".to_vec()));
    v.push(("bad-opcode", ok(&[0xff, 0x0b])));
    v.push(("simd", ok(&[0xfd, 0x0c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1a, 0x41, 0x00, 0x0b])));
    v.push(("type-error", ok(&[0x42, 0x00, 0x0b])));
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        m.extra_sections.push((0, 14, vec![0x00]));
        v.push(("unknown-section", m.encode()));
    }
    {
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        m.extra_sections.push((99, 0, b"\x04namexyz".to_vec()));
        v.push(("custom-section", m.encode()));
    }
    {
        // a start section placed before the export section (out of order)
        let mut m = abi(MANIFEST, 1, &[], &[0x41, 0x00, 0x0b]);
        let s = m.func(&[], &[], &[], &[0x0b]);
        let mut p = Vec::new();
        uleb(&mut p, s as u64);
        m.extra_sections.push((5, 8, p));
        v.push(("start-out-of-order", m.encode()));
    }
    v
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

fn surface(fmt: PixelFormat, w: i32, h: i32) -> Surface {
    let mut s = Surface::new(fmt);
    for y in 0..h {
        for x in 0..w {
            let v = ((x * 7 + y * 13) % 31) as f32 / 30.0;
            let a = if (x + y) % 9 == 0 { 0.0 } else { 0.25 + 0.75 * ((x % 4) as f32 / 3.0) };
            s.write_pixel(x, y, &[v, 1.0 - v, (x % 5) as f32 / 4.0, a]);
        }
    }
    s
}

fn selection() -> Surface {
    let mut sel = Surface::new(PixelFormat::GRAY8);
    sel.fill_rect(Rect::new(0, 0, 20, 30), &[1.0]);
    sel.fill_rect(Rect::new(0, 0, 20, 10), &[0.5]);
    sel
}

fn surface_result(s: &Surface, canvas: Rect) -> String {
    let data = s.read_region(canvas);
    let mut bytes = Vec::with_capacity(data.len() * 4);
    for v in data {
        bytes.extend_from_slice(&v.to_le_bytes());
    }
    let b = s.content_bounds();
    format!("ok\t{}\t{},{},{},{}\t{}", s.tile_count(), b.x0, b.y0, b.x1, b.y1, &blake3::hash(&bytes).to_hex()[..32])
}

fn limits(tag: &str) -> Limits {
    let d = Limits::default();
    match tag {
        "default" => d,
        "module100" => Limits { max_module_bytes: 100, ..d },
        "mem1024" => Limits { max_memory_bytes: 1024, ..d },
        "mem4m" => Limits { max_memory_bytes: 4 << 20, band_bytes: 64 << 20, ..d },
        "fuel1m" => Limits { fuel_base: 1_000_000, fuel_per_sample: 10, ..d },
        "wall300" => Limits { fuel_base: u64::MAX / 4, wall_time_ms: 300, ..d },
        "band1" => Limits { band_bytes: 1, ..d },
        "small" => Limits { band_bytes: 40 * 4 * 4 * 3, ..d },
        "depth16" => Limits { max_recursion_depth: 16, ..d },
        _ => d,
    }
}

fn fmt_by_name(n: &str) -> PixelFormat {
    match n {
        "rgba8" => PixelFormat::RGBA8,
        "rgba16" => PixelFormat::RGBA16,
        "rgba32f" => PixelFormat::RGBA32F,
        "gray8" => PixelFormat::GRAY8,
        "graya8" => PixelFormat::GRAYA8,
        "cmyka8" => PixelFormat::CMYKA8,
        "lab16" => PixelFormat { mode: ColorMode::Lab, sample: SampleType::U16, alpha: true },
        "multi8" => PixelFormat { mode: ColorMode::Multichannel, sample: SampleType::U8, alpha: false },
        "duotone32f" => PixelFormat { mode: ColorMode::Duotone, sample: SampleType::F32, alpha: true },
        _ => PixelFormat::RGBA8,
    }
}

/// CASE = FORMAT:W:H[:sel|:empty][:params=JSON] (the JSON is the rest of the case)
fn apply_case(p: &Plugin, case: &str) -> String {
    let (head, params) = match case.find(":params=") {
        Some(i) => (&case[..i], serde_json::from_str(&case[i + 8..]).unwrap_or(Value::Null)),
        None => (case, json!({})),
    };
    let parts: Vec<&str> = head.split(':').collect();
    let fmt = fmt_by_name(parts[0]);
    let (w, h): (i32, i32) = (parts[1].parse().unwrap_or(1), parts[2].parse().unwrap_or(1));
    let sel = if parts.contains(&"sel") { Some(selection()) } else { None };
    let s = if parts.contains(&"empty") { Surface::new(fmt) } else { surface(fmt, w, h) };
    let canvas = Rect::new(0, 0, w, h);
    match catch_unwind(AssertUnwindSafe(|| p.apply(&s, canvas, sel.as_ref(), &params))) {
        Ok(Ok(out)) => surface_result(&out, canvas),
        Ok(Err(e)) => err_text(&e),
        Err(_) => "panic".into(),
    }
}

// ---------------------------------------------------------------------------
// Manifests
// ---------------------------------------------------------------------------

const FULL: &str = r#"{"id":"org.example.tone","name":"Tone","version":"1.2","kind":"filter",
        "params":{"amount":{"type":"number","min":0,"max":100,"default":50},
                  "mode":{"type":"choice","options":["soft","hard"],"default":"hard"},
                  "mono":{"type":"bool"},"seed":{"type":"int","min":0,"max":9}},
        "overlap":2,"area":"canvas"}"#;

const RESOLVE_MANIFESTS: &[&str] = &[
    FULL,
    r#"{"id":"org.example.tone","name":"Tone","kind":"filter",
        "params":{"amount":{"type":"number","min":0,"max":100,"default":1000},
                  "steps":{"type":"int","min":1,"max":16,"default":1000},
                  "low":{"type":"number","min":-1,"max":1,"default":-5},
                  "count":{"type":"int","min":3,"max":9}}}"#,
    r#"{"id":"x","name":"X","kind":"filter","params":{"c":{"type":"choice","options":["b","a"]},
        "i":{"type":"int"},"Z9":{"type":"number","min":-1e300,"max":1e300,"default":0.1},
        "dup":{"type":"bool"},"dup":{"type":"int","min":-5,"max":5,"default":-7}}}"#,
];

fn manifest_inputs() -> Vec<Vec<u8>> {
    let mut v: Vec<Vec<u8>> = Vec::new();
    let s = |t: &str| t.as_bytes().to_vec();
    v.push(s(FULL));
    for m in RESOLVE_MANIFESTS {
        v.push(s(m));
    }
    for bad in [
        "",
        "[]",
        "not json",
        r#"{"id":"","name":"x","kind":"filter"}"#,
        r#"{"id":"a b","name":"x","kind":"filter"}"#,
        r#"{"id":"a","name":"","kind":"filter"}"#,
        r#"{"id":"a","name":"x","kind":"panel"}"#,
        r#"{"id":"a","name":"x"}"#,
        r#"{"id":"a","name":"x","kind":"filter","overlap":100000}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"r":{"type":"number","min":5,"max":1,"default":1}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"c":{"type":"choice","options":[]}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"__x":{"type":"bool"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":[1]}"#,
    ] {
        v.push(s(bad));
    }
    v.push(vec![0xff, 0xfe]);
    // (64 KiB inputs are checked by tests/plugins/test_manifest.c rather than stored here)
    let with_name = |n: &str| json!({"id": "org.example.tone", "name": n, "kind": "filter"}).to_string().into_bytes();
    for n in [
        "Tone\u{202E}evil",
        "\u{2067}Tone\u{2069}",
        "Tone\u{200F}",
        "\u{200B}\u{200B}\u{200B}",
        "Tone\u{00AD}",
        "\u{FEFF}Tone",
        "\u{200D}",
        "\u{3164}\u{FE0F}",
        " \u{3000} ",
        "Tone\u{7}",
        "Tone…",
        "Kontrast ±",
        "می\u{200C}خواهم",
        "\u{1F469}\u{200D}\u{1F4BB} Code",
        "\u{2764}\u{FE0F} Love",
        "色调",
        "  padded  ",
        "\u{85}Next",
        "a\u{9f}",
        "\u{E0041}tag",
    ] {
        v.push(with_name(n));
    }
    v.push(with_name(&"é".repeat(64)));
    v.push(with_name(&"é".repeat(65)));
    v.push(with_name(&"x".repeat(64)));
    for t in [
        "{}",
        r#"{"id":5}"#,
        r#"{"id":null,"name":"x","kind":"filter"}"#,
        r#"{"id":true}"#,
        r#"{"id":[1]}"#,
        r#"{"id":{"a":1}}"#,
        r#"{"id":-3}"#,
        r#"{"id":1.5}"#,
        r#"{"id":-0}"#,
        r#"{"id":1e5}"#,
        r#"{"id":"a","name":"x","kind":"filter","overlap":-1}"#,
        r#"{"id":"a","name":"x","kind":"filter","overlap":1.5}"#,
        r#"{"id":"a","name":"x","kind":"filter","overlap":5000000000}"#,
        r#"{"id":"a","name":"x","kind":"filter","overlap":"3"}"#,
        r#"{"id":"a","name":"x","kind":"filter","overlap":256}"#,
        r#"{"id":"a","name":"x","kind":"filter","overlap":257}"#,
        r#"{"id":"a","name":"x","kind":"filter","overlap":18446744073709551616}"#,
        r#"{"id":"a","name":"x","kind":{"filter":null}}"#,
        r#"{"id":"a","name":"x","kind":{"filter":1}}"#,
        r#"{"id":"a","name":"x","kind":{"filter":null,"x":2}}"#,
        r#"{"id":"a","name":"x","kind":{}}"#,
        r#"{"id":"a","name":"x","kind":{"panel":null}}"#,
        r#"{"id":"a","name":"x","kind":5}"#,
        r#"{"id":"a","name":"x","kind":null}"#,
        r#"{"id":"a","name":"x","kind":["filter"]}"#,
        r#"{"id":"a","name":"x","kind":"FILTER"}"#,
        r#"{"id":"a","name":"x","kind":"filter","area":"middle"}"#,
        r#"{"id":"a","name":"x","kind":"filter","area":"content"}"#,
        r#"{"id":"a","name":"x","kind":"filter","area":3}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":null}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":"p"}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":7}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":-7.25}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":true}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":[ ]}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":5}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":null}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":"number"}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"slider"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"slider"},"b":1}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":5}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"min":1}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"number"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"number","min":0}}, "b": 1}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"number","min":0,"max":1}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"number","min":"x","max":1,"default":0}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"number","min":null,"max":1,"default":0}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"number","min":[],"max":1,"default":0}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"number","min":{},"max":1,"default":0}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"number","min":true,"max":1,"default":0}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"number","min":-1,"max":1,"default":0.5}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"number","min":0,"max":1,"default":0,"min":2}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"number","type":"int"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"int","min":1.5}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"int","max":18446744073709551615}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"int","max":9223372036854775807,"min":-9223372036854775808}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"int","min":5,"max":1}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"int","default":"1"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"bool","default":1}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"bool","default":true,"extra":[1,{"b":null}]}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"choice","options":"x"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"choice","options":[1]}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"choice","options":["a"],"default":null}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"choice","options":["a"],"default":"b"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"choice","options":["a"],"default":3}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"choice","options":["a|b"]}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"choice","options":["a\"b"]}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"choice","options":[""]}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"choice","options":["x","y","z"],"default":"z"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":["number",0,10,5]}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":["number",0]}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":["int"]}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":["number",0,1,0,9]}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":["choice"]}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":["choice",["p","q"],"q"]}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":["bool",true,1]}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":[]}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":[7]}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"_x":{"type":"bool"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"id":{"type":"bool"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"layer":{"type":"bool"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a-b":{"type":"bool"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"é":{"type":"bool"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"":{"type":"bool"}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"bool"},"a":{"type":"int"}}}"#,
        r#"{"id":"a","id":"b","name":"x","kind":"filter"}"#,
        r#"{"id":"a","name":"x","kind":"filter","name":"y"}"#,
        r#"{"id":"a","name":"x","kind":"filter","unknown":[1,2,{"x":[]}],"more":null}"#,
        r#"{"id":"a","name":"x","kind":"filter"} x"#,
        r#"{"id":"a","name":"x","kind":"filter"}   "#,
        r#"{"id":"a","name":"x","kind":"filter"}}"#,
        r#"{"id":"a","#,
        r#"{"id" "a"}"#,
        r#"{"id":"a",}"#,
        r#"{,}"#,
        r#"{"id":"a" "name":"x"}"#,
        r#"{"id":"a","name":"x","kind":"filter","unknown":[1,]}"#,
        r#"{"id":"a","name":"x","kind":"filter","unknown":{"a":1,}}"#,
        r#"{"id":"a","name":"x","kind":"filter","unknown":[1 2]}"#,
        r#"{"id":"a","name":"x","kind":"filter","unknown":tru}"#,
        r#"{"id":"a","name":"x","kind":"filter","unknown":nulx}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"bool","x":[1,]}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"bool","x":[1 2]}}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a":{"type":"bool"},}}"#,
        r#"{"id":"a","name":"x","kind":"filter","params":{"a" {"type":"bool"}}}"#,
        r#"{"id":"\u0061","name":"\u00e9\ud83d\ude00","kind":"filter"}"#,
        r#"{"id":"a","name":"x\ud800","kind":"filter"}"#,
        r#"{"id":"a","name":"x","kind":"filter","version":"\x"}"#,
        "{\"id\":\"a\",\"name\":\"x\u{1}\",\"kind\":\"filter\"}",
        r#"{"id":"a","name":"x","kind":"filter","overlap":1e400}"#,
        r#"{"id":"a","name":"x","kind":"filter","overlap":01}"#,
        r#"{"id":"a","name":"x","kind":"filter","overlap":-}"#,
        r#"{"id":"a","name":"x","kind":"filter","overlap":1.}"#,
        "{\n  \"id\": 5\n}",
        "\n\n  {\"id\":\"a\",\n\"name\":\"x\"\n,\"kind\":\"pane\"}",
        "{\"id\":\"a\",\n\"name\":\"x\",\n\"kind\":\"filter\"}\n\n",
        "\u{feff}{\"id\":\"a\",\"name\":\"x\",\"kind\":\"filter\"}",
        r#"["a","x"]"#,
        r#"["a","x","1","filter"]"#,
        r#"["a","x","1","filter","d","au",null,0,"canvas"]"#,
        r#"["a","x","1","filter","d","au",{"p":{"type":"bool"}},3,"canvas","extra"]"#,
        r#"["a","x","1","filter",]"#,
        r#"["a"]"#,
        r#"[5]"#,
        r#"[]"#,
        r#""str""#,
        "null",
        "1",
        "-1",
        "true",
        "  ",
        r#"{"id":"a\"b","name":"x","kind":"filter"}"#,
        r#"{"id":"a\u202eb","name":"x","kind":"filter"}"#,
        r#"{"id":"é","name":"x","kind":"filter"}"#,
        r#"{"id":"a\u0000","name":"x","kind":"filter"}"#,
        r#"{"id":"a","name":"x","kind":"filter","version":"123456789012345678901234567890123"}"#,
        r#"{"id":"a","name":"x","kind":"filter","version":"12345678901234567890123456789012"}"#,
    ] {
        v.push(s(t));
    }
    let id64 = "a".repeat(64);
    v.push(json!({"id": id64, "name": "x", "kind": "filter"}).to_string().into_bytes());
    v.push(json!({"id": "a".repeat(65), "name": "x", "kind": "filter"}).to_string().into_bytes());
    v.push(json!({"id": "a", "name": "x", "kind": "filter", "description": "d".repeat(1025)}).to_string().into_bytes());
    v.push(json!({"id": "a", "name": "x", "kind": "filter", "author": "d".repeat(129)}).to_string().into_bytes());
    v.push(json!({"id": "a", "name": "x", "kind": "filter", "author": "d".repeat(128)}).to_string().into_bytes());
    // 64 and 65 parameters
    for n in [64, 65] {
        let mut p = String::from("{");
        for i in 0..n {
            if i > 0 {
                p.push(',');
            }
            p.push_str(&format!("\"p{i}\":{{\"type\":\"bool\"}}"));
        }
        p.push('}');
        v.push(format!(r#"{{"id":"a","name":"x","kind":"filter","params":{p}}}"#).into_bytes());
    }
    // 64 and 65 choices
    for n in [64, 65] {
        let opts: Vec<String> = (0..n).map(|i| format!("o{i}")).collect();
        v.push(json!({"id": "a", "name": "x", "kind": "filter", "params": {"c": {"type": "choice", "options": opts}}})
            .to_string()
            .into_bytes());
    }
    // a 65-byte parameter name
    v.push(format!(r#"{{"id":"a","name":"x","kind":"filter","params":{{"{}":{{"type":"bool"}}}}}}"#, "k".repeat(65)).into_bytes());
    // nesting: ignored values have no depth limit; buffered (Content) values do
    for depth in [100usize, 125, 126, 127, 200] {
        let deep = format!("{}{}", "[".repeat(depth), "]".repeat(depth));
        v.push(format!(r#"{{"id":"a","name":"x","kind":"filter","unknown":{deep}}}"#).into_bytes());
        v.push(format!(r#"{{"id":"a","name":"x","kind":"filter","params":{{"a":{{"type":"bool","x":{deep}}}}}}}"#).into_bytes());
    }
    v
}

const RESOLVE_PARAMS: &[&str] = &[
    "null",
    "{}",
    "[1,2]",
    "\"x\"",
    "1",
    "true",
    r#"{"amount": 500, "seed": 3.6, "junk": 1}"#,
    r#"{"amount": "lots"}"#,
    r#"{"mode": "medium"}"#,
    r#"{"mono": 1}"#,
    r#"{"amount": null, "steps": null}"#,
    r#"{"amount": 500, "steps": 500, "low": 0.5, "count": 5}"#,
    r#"{"amount": -0.0, "seed": -0.5}"#,
    r#"{"amount": 1e300, "seed": 1e300, "steps": -1e300}"#,
    r#"{"seed": 2.5, "count": 4.5, "steps": -2.5}"#,
    r#"{"mode": "soft", "mono": true}"#,
    r#"{"mode": 5}"#,
    r#"{"mode": null, "mono": null}"#,
    r#"{"amount": 1, "amount": 2}"#,
    r#"{"c": "a", "i": 1000001, "Z9": -5}"#,
    r#"{"c": "c"}"#,
    r#"{"c": ["a"]}"#,
    r#"{"i": "1"}"#,
    r#"{"dup": 3}"#,
    r#"{"dup": true}"#,
    r#"{"i": 18446744073709551615, "Z9": 18446744073709551615}"#,
    r#"{"i": -9223372036854775808}"#,
    r#"{"seed": 1e-300}"#,
];

fn plugin_line(p: &Plugin) -> String {
    format!("ok\t{}\t{}\t{}\t{}", esc(p.id()), esc(&p.manifest().name), esc(&p.manifest().params_notation()), p.size())
}

pub fn run() {
    // ---- manifests ----
    for input in manifest_inputs() {
        let r = match Manifest::parse(&input) {
            Ok(m) => format!("ok\t{}\t{}", esc(&m.params_notation()), esc(&serde_json::to_string(&m).unwrap_or_default())),
            Err(e) => format!("err\t{}", esc(&e.to_string())),
        };
        println!("man\t{}\t{r}", hex(&input));
    }
    for (mi, m) in RESOLVE_MANIFESTS.iter().enumerate() {
        println!("resman\t{mi}\t{}", hex(m.as_bytes()));
        let Ok(man) = Manifest::parse(m.as_bytes()) else {
            println!("res\t{mi}\t-\tbad manifest");
            continue;
        };
        for p in RESOLVE_PARAMS {
            let v: Value = serde_json::from_str(p).unwrap_or(Value::Null);
            let r = match man.resolve_params(&v) {
                Ok(m) => format!("ok\t{}", esc(&Value::Object(m).to_string())),
                Err(e) => format!("err\t{}", esc(&e.to_string())),
            };
            println!("res\t{mi}\t{}\t{r}", hex(p.as_bytes()));
        }
    }
    // ---- format codes ----
    for mode in [
        ColorMode::Bitmap,
        ColorMode::Grayscale,
        ColorMode::Indexed,
        ColorMode::Rgb,
        ColorMode::Cmyk,
        ColorMode::Lab,
        ColorMode::Multichannel,
        ColorMode::Duotone,
    ] {
        for sample in SampleType::ALL {
            for alpha in [false, true] {
                println!("fmt\t{mode:?}\t{sample:?}\t{alpha}\t{}", format_code(PixelFormat { mode, sample, alpha }));
            }
        }
    }
    // ---- modules ----
    let mods = modules();
    for (n, b) in &mods {
        println!("mod\t{n}\t{}", hex(b));
    }
    let get = |n: &str| mods.iter().find(|m| m.0 == n).map(|m| m.1.clone()).unwrap_or_default();
    // loads under each limit set
    let load_cases: Vec<(&str, &str)> = mods
        .iter()
        .map(|m| (m.0, "default"))
        .chain([("invert", "module100"), ("invert", "mem1024"), ("invert", "mem4m"), ("start-ok", "depth16")])
        .collect();
    for (n, lt) in &load_cases {
        let b = get(n);
        let r = match catch_unwind(AssertUnwindSafe(|| Plugin::load(&b, limits(lt)))) {
            Ok(Ok(p)) => plugin_line(&p),
            Ok(Err(e)) => err_text(&e),
            Err(_) => "panic".into(),
        };
        println!("load\t{n}\t{lt}\t{r}");
    }
    // applies
    let mut apply_cases: Vec<(&str, &str, String)> = Vec::new();
    for f in ["rgba8", "rgba16", "rgba32f", "graya8", "gray8", "cmyka8", "lab16", "multi8", "duotone32f"] {
        apply_cases.push(("invert", "default", format!("{f}:40:30")));
        apply_cases.push(("invert", "default", format!("{f}:40:30:sel")));
    }
    apply_cases.push(("invert", "small", "rgba16:40:30".into()));
    apply_cases.push(("invert", "band1", "rgba8:600:300".into()));
    apply_cases.push(("invert", "default", "rgba8:600:300".into()));
    apply_cases.push(("invert", "default", "rgba8:40:30:empty".into()));
    apply_cases.push(("invert", "mem4m", "rgba8:64:8".into()));
    apply_cases.push(("invert", "default", "rgba8:40:30:params=[1]".into()));
    apply_cases.push(("invert", "default", "rgba8:40:30:params={\"x\":1}".into()));
    for p in ["{}", "{\"amount\":3}", "{\"amount\":\"x\"}", "{\"amount\":1e300}", "null"] {
        apply_cases.push(("params", "default", format!("rgba32f:8:8:params={p}")));
    }
    apply_cases.push(("params", "band1", "rgba32f:300:20".into()));
    for f in ["rgba8", "rgba32f", "gray8", "cmyka8"] {
        apply_cases.push(("nan", "default", format!("{f}:8:8")));
        apply_cases.push(("sanitize", "default", format!("{f}:12:9")));
        apply_cases.push(("args", "default", format!("{f}:12:9")));
    }
    apply_cases.push(("sanitize", "default", "rgba16:12:9:sel".into()));
    for (f, lt) in [("rgba8", "default"), ("rgba32f", "default"), ("rgba8", "band1"), ("graya8", "small")] {
        apply_cases.push(("shift", lt, format!("{f}:300:40")));
        apply_cases.push(("shift", lt, format!("{f}:300:40:sel")));
    }
    apply_cases.push(("canvas", "default", "rgba8:50:40:empty".into()));
    apply_cases.push(("canvas", "default", "rgba32f:50:40".into()));
    apply_cases.push(("canvas", "band1", "graya8:300:270:sel".into()));
    apply_cases.push(("fill0", "default", "rgba8:20:20".into()));
    apply_cases.push(("noop", "default", "rgba16:20:20".into()));
    apply_cases.push(("start-ok", "default", "rgba8:20:20".into()));
    apply_cases.push(("tail-call", "default", "rgba8:20:20".into()));
    apply_cases.push(("ctors-loop", "default", "rgba8:20:20".into()));
    apply_cases.push(("indirect-ok", "default", "rgba8:20:20".into()));
    for n in [
        "spin",
        "grow-bomb",
        "grow-forever",
        "oob-write",
        "trap",
        "div-zero",
        "int-overflow",
        "bad-conversion",
        "recursion",
        "error-code",
        "bogus-alloc",
        "alloc-zero",
        "alloc-traps",
        "indirect-null",
        "indirect-oob",
        "indirect-mismatch",
    ] {
        apply_cases.push((n, "default", "rgba8:32:24".into()));
    }
    apply_cases.push(("spin", "fuel1m", "rgba8:32:24".into()));
    apply_cases.push(("spin", "wall300", "rgba8:32:24".into()));
    apply_cases.push(("recursion", "depth16", "rgba8:32:24".into()));
    apply_cases.push(("invert", "mem1024", "rgba8:64:8".into()));
    for (n, lt, case) in &apply_cases {
        let b = get(n);
        let r = match Plugin::load(&b, limits(lt)) {
            Ok(p) => apply_case(&p, case),
            Err(e) => format!("load-{}", err_text(&e)),
        };
        println!("apply\t{n}\t{lt}\t{case}\t{r}");
    }
    // output areas
    if let (Ok(inv), Ok(shift), Ok(canvas)) =
        (Plugin::load(&get("invert"), Limits::default()), Plugin::load(&get("shift"), Limits::default()), Plugin::load(&get("canvas"), Limits::default()))
    {
        let s = surface(PixelFormat::RGBA8, 40, 30);
        let e = Surface::new(PixelFormat::RGBA8);
        let sel = selection();
        let mut far = Surface::new(PixelFormat::GRAY8);
        far.fill_rect(Rect::new(100, 100, 120, 120), &[1.0]);
        for (pn, p) in [("invert", &inv), ("shift", &shift), ("canvas", &canvas)] {
            for (cn, surf, canvas_r, selection) in [
                ("full", &s, Rect::new(0, 0, 40, 30), None),
                ("small-canvas", &s, Rect::new(0, 0, 20, 10), None),
                ("offset-canvas", &s, Rect::new(-5, -5, 10, 10), None),
                ("empty", &e, Rect::new(0, 0, 40, 30), None),
                ("sel", &s, Rect::new(0, 0, 40, 30), Some(&sel)),
                ("far-sel", &s, Rect::new(0, 0, 40, 30), Some(&far)),
            ] {
                let r = p.output_area(surf, canvas_r, selection);
                println!("area\t{pn}\t{cn}\t{}\t{}\t{}\t{}", r.x0, r.y0, r.x1, r.y1);
            }
        }
    }
    // ---- registry ----
    let rev0 = registry::revision();
    let mk = |id: &str| abi(&format!(r#"{{"id":"{id}","name":"R {id}","kind":"filter"}}"#), 1, &[], &[0x41, 0x00, 0x0b]).encode();
    let show = |step: &str| {
        let ids: Vec<String> = registry::list().iter().map(|p| format!("{}={}", p.id(), p.manifest().name)).collect();
        println!("reg\t{step}\t{}\t{}", ids.join(","), registry::revision() - rev0);
    };
    let r = registry::install_bytes(&mk("test.b")).map(|p| p.id().to_string());
    println!("reg\tinstall-b\t{r:?}");
    let r = registry::install_bytes(&mk("test.a")).map(|p| p.id().to_string());
    println!("reg\tinstall-a\t{r:?}");
    show("after-installs");
    let r = registry::install_bytes(&abi(r#"{"id":"test.a","name":"Again","kind":"filter"}"#, 1, &[], &[0x41, 0x00, 0x0b]).encode())
        .map(|p| p.id().to_string());
    println!("reg\treinstall-a\t{r:?}");
    show("after-reinstall");
    println!("reg\tbad\t{}", registry::install_bytes(b"\0asm junk").map(|_| ()).map_err(|e| e.to_string()).err().unwrap_or_default());
    show("after-bad");
    println!("reg\tget-a\t{}", registry::get("test.a").map(|p| p.manifest().name.clone()).unwrap_or_default());
    println!("reg\tget-z\t{}", registry::get("test.z").is_some());
    println!("reg\tremove-a\t{}", registry::remove("test.a"));
    println!("reg\tremove-a-again\t{}", registry::remove("test.a"));
    show("after-remove");
    println!("reg\tremove-b\t{}", registry::remove("test.b"));
    show("end");
    // ---- folders ----
    let dir = std::env::temp_dir().join(format!("photocraft-rustref-plugins-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    if std::fs::create_dir_all(&dir).is_ok() {
        let files: Vec<(&str, Vec<u8>)> = vec![
            ("b-invert.wasm", INVERT.to_vec()),
            ("a-broken.wasm", b"\0asm junk".to_vec()),
            ("c-params.WASM", get("params")),
            ("notes.txt", b"ignored".to_vec()),
            (".wasm", get("noop")),
            ("d-empty.wasm", Vec::new()),
            ("e-import.Wasm", get("wasi-import")),
        ];
        for (n, b) in &files {
            let _ = std::fs::write(dir.join(n), b);
        }
        let _ = std::fs::create_dir(dir.join("f-dir.wasm"));
        let prefix = format!("{}/", dir.display());
        match registry::load_folder(&dir) {
            Ok(r) => {
                println!("folder\tloaded\t{}", r.loaded.join(","));
                for (f, e) in &r.failed {
                    println!("folder\tfailed\t{}\t{}", esc(&f.replace(&prefix, "")), esc(&e.replace(&prefix, "")));
                }
            }
            Err(e) => println!("folder\terror\t{}", esc(&e.to_string())),
        }
        let r = registry::load_file(&dir, Limits::default()).map(|_| ()).map_err(|e| e.to_string().replace(&format!("{}", dir.display()), "DIR"));
        println!("folder\tfile-dir\t{}", esc(&r.err().unwrap_or_default()));
        let r = registry::load_file(&dir.join("b-invert.wasm"), Limits { max_module_bytes: 100, ..Limits::default() })
            .map(|_| ())
            .map_err(|e| e.to_string().replace(&prefix, ""));
        println!("folder\tfile-big\t{}", esc(&r.err().unwrap_or_default()));
        let r = registry::load_file(&dir.join("b-invert.wasm"), Limits::default()).map(|p| p.source().map(|s| s.replace(&prefix, "")));
        println!("folder\tfile-source\t{:?}", r.ok().flatten());
        for id in registry::list() {
            registry::remove(id.id());
        }
        let _ = std::fs::remove_dir_all(&dir);
    }
    for missing in ["/nonexistent-photocraft-plugins", "/nonexistent-photocraft-plugins/x.wasm"] {
        let r = registry::load_folder(std::path::Path::new(missing)).map(|_| ()).map_err(|e| e.to_string());
        println!("folder\tmissing-dir\t{missing}\t{}", esc(&r.err().unwrap_or_default()));
        let r = registry::load_file(std::path::Path::new(missing), Limits::default()).map(|_| ()).map_err(|e| e.to_string());
        println!("folder\tmissing-file\t{missing}\t{}", esc(&r.err().unwrap_or_default()));
    }
}
