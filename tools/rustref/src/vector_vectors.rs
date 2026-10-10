//! L12 vector reference vectors (`rustref vector-vectors`). Owned by that chunk's port.
//!
//! One line per case, `tag args... value`; hashes are FNV-1a 64 over little-endian bit patterns
//! (f64/f32 bits, u64 counts, u8 flags). tests/vector/test_parity.c builds the same inputs with
//! the same LCG and reproduces every line.
use photocraft_color::{Color, ColorMode, PixelFormat, SampleType};
use photocraft_doc::{Fill, FillRule, GradientStyle, Knot, LineCap, LineJoin, LiveShape, Path, PathOp, ShapeLayer, ShapeStroke, StrokeAlign, Subpath, VectorMask};
use photocraft_geom::{Point, Rect};
use photocraft_vector::edit::{self, Handle, Hit};
use photocraft_vector::stroke::dash_polyline;
use photocraft_vector::*;

const H0: u64 = 0xcbf29ce484222325;

struct H(u64);
impl H {
    fn new() -> H {
        H(H0)
    }
    fn b(&mut self, b: &[u8]) {
        for &x in b {
            self.0 ^= x as u64;
            self.0 = self.0.wrapping_mul(0x100000001b3);
        }
    }
    fn f64(&mut self, v: f64) {
        self.b(&v.to_bits().to_le_bytes());
    }
    fn f32(&mut self, v: f32) {
        self.b(&v.to_bits().to_le_bytes());
    }
    fn u64(&mut self, v: u64) {
        self.b(&v.to_le_bytes());
    }
    fn flag(&mut self, v: bool) {
        self.b(&[v as u8]);
    }
}

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        self.0
    }
    /// Uniform in [0, 1).
    fn f(&mut self) -> f64 {
        (self.next() >> 11) as f64 / 9007199254740992.0
    }
    fn below(&mut self, n: u64) -> u64 {
        (self.next() >> 33) % n
    }
}

fn op_of(i: u64) -> PathOp {
    [PathOp::Combine, PathOp::Subtract, PathOp::Intersect, PathOp::Exclude, PathOp::Join][i as usize % 5]
}
fn op_idx(o: PathOp) -> u8 {
    match o {
        PathOp::Combine => 0,
        PathOp::Subtract => 1,
        PathOp::Intersect => 2,
        PathOp::Exclude => 3,
        PathOp::Join => 4,
    }
}
const CAPS: [LineCap; 3] = [LineCap::Butt, LineCap::Round, LineCap::Square];
const JOINS: [LineJoin; 3] = [LineJoin::Miter, LineJoin::Round, LineJoin::Bevel];
const STYLES: [GradientStyle; 5] = [GradientStyle::Linear, GradientStyle::Radial, GradientStyle::Angle, GradientStyle::Reflected, GradientStyle::Diamond];

fn hash_path(h: &mut H, p: &Path) {
    h.u64(p.subpaths.len() as u64);
    h.flag(p.fill_rule == FillRule::EvenOdd);
    h.flag(p.inverted);
    for s in &p.subpaths {
        h.flag(s.closed);
        h.b(&[op_idx(s.op)]);
        h.u64(s.knots.len() as u64);
        for k in &s.knots {
            for q in [k.anchor, k.in_ctrl, k.out_ctrl] {
                h.f64(q.x);
                h.f64(q.y);
            }
            h.flag(k.smooth);
        }
    }
}

fn path_hash(p: &Path) -> u64 {
    let mut h = H::new();
    hash_path(&mut h, p);
    h.0
}

fn hash_polys(h: &mut H, v: &[Vec<(f64, f64)>]) -> usize {
    let mut n = 0;
    h.u64(v.len() as u64);
    for p in v {
        h.u64(p.len() as u64);
        n += p.len();
        for q in p {
            h.f64(q.0);
            h.f64(q.1);
        }
    }
    n
}

fn hash_lines(h: &mut H, v: &[Polyline]) -> usize {
    let mut n = 0;
    h.u64(v.len() as u64);
    for p in v {
        h.flag(p.closed);
        h.u64(p.pts.len() as u64);
        n += p.pts.len();
        for (q, k) in p.pts.iter().zip(&p.knot) {
            h.f64(q.0);
            h.f64(q.1);
            h.flag(*k);
        }
    }
    n
}

fn cov_hash(v: &[f32]) -> u64 {
    let mut h = H::new();
    for x in v {
        h.f32(*x);
    }
    h.0
}

fn bounds_str(b: Option<Rect>) -> String {
    match b {
        Some(r) => format!("{},{},{},{}", r.x0, r.y0, r.x1, r.y1),
        None => "none".into(),
    }
}

/// A random path: `nsub` subpaths of `nk` knots in [0, scale)², a mix of corners, smooth
/// knots and free handles.
fn rand_path(rng: &mut Rng, nsub: usize, nk: usize, scale: f64) -> Path {
    let mut subs = Vec::new();
    for _ in 0..nsub {
        let closed = rng.f() < 0.75;
        let op = op_of(rng.below(5));
        let mut knots = Vec::new();
        for _ in 0..nk {
            let a = Point::new(rng.f() * scale, rng.f() * scale);
            let kind = rng.below(3);
            let k = match kind {
                0 => Knot::corner(a.x, a.y),
                1 => {
                    let (dx, dy) = ((rng.f() - 0.5) * scale * 0.3, (rng.f() - 0.5) * scale * 0.3);
                    Knot::smooth(a, Point::new(a.x - dx, a.y - dy), Point::new(a.x + dx, a.y + dy))
                }
                _ => {
                    let i = Point::new(a.x + (rng.f() - 0.5) * scale * 0.4, a.y + (rng.f() - 0.5) * scale * 0.4);
                    let o = Point::new(a.x + (rng.f() - 0.5) * scale * 0.4, a.y + (rng.f() - 0.5) * scale * 0.4);
                    Knot { anchor: a, in_ctrl: i, out_ctrl: o, smooth: false }
                }
            };
            knots.push(k);
        }
        subs.push(Subpath { closed, knots, op });
    }
    let mut p = Path::new(subs);
    if rng.below(4) == 0 {
        p.fill_rule = FillRule::EvenOdd;
    }
    if rng.below(6) == 0 {
        p.inverted = true;
    }
    p
}

fn pentagram() -> Path {
    let pts: Vec<(f64, f64)> = (0..5)
        .map(|i| {
            let a = -std::f64::consts::FRAC_PI_2 + std::f64::consts::TAU * (i * 2 % 5) as f64 / 5.0;
            (50.0 + 40.0 * a.cos(), 50.0 + 40.0 * a.sin())
        })
        .collect();
    Path::new(vec![Subpath::polygon(&pts)])
}

fn two_squares(op: PathOp) -> Path {
    Path::new(vec![shapes::rect(10.0, 10.0, 40.0, 40.0).subpaths.remove(0), shapes::rect(30.0, 30.0, 40.0, 40.0).subpaths.remove(0).with_op(op)])
}

fn curve() -> Path {
    let mut sp = Subpath::polyline(&[(0.0, 50.0), (50.0, 0.0), (100.0, 50.0)]);
    sp.knots[1] = Knot::smooth(Point::new(50.0, 0.0), Point::new(30.0, 0.0), Point::new(70.0, 0.0));
    Path::new(vec![sp])
}

fn tri() -> Path {
    Path::new(vec![Subpath::polygon(&[(0.0, 0.0), (100.0, 0.0), (50.0, 80.0)])])
}

/// The fixed path corpus (index = line id).
fn corpus() -> Vec<Path> {
    let mut v = vec![
        shapes::rect(10.3, 20.7, 100.45, 50.2),
        shapes::ellipse(10.0, 10.0, 80.0, 80.0),
        shapes::rounded_rect(5.0, 5.0, 60.0, 40.0, [10.0, 0.0, 15.0, 3.0]),
        shapes::polygon(3.0, 5.0, 90.0, 80.0, 7, 0.45),
        pentagram(),
    ];
    let mut eo = pentagram();
    eo.fill_rule = FillRule::EvenOdd;
    v.push(eo);
    for i in 0..5 {
        v.push(two_squares(op_of(i)));
    }
    let mut inv = two_squares(PathOp::Intersect);
    inv.inverted = true;
    v.push(inv);
    let mut d = shapes::ellipse(10.0, 10.0, 80.0, 80.0);
    d.subpaths.push(shapes::ellipse(30.0, 30.0, 40.0, 40.0).subpaths.remove(0).with_op(PathOp::Subtract));
    v.push(d);
    v.push(curve());
    v.push(tri());
    // Degenerate and hostile inputs.
    v.push(Path::default());
    v.push(Path::new(vec![Subpath::polygon(&[(5.0, 5.0)])]));
    v.push(Path::new(vec![Subpath::polygon(&[(5.0, 5.0), (5.0, 5.0)])]));
    v.push(Path::new(vec![Subpath::polygon(&[(5.0, 5.0), (f64::NAN, 9.0), (40.0, 30.0), (10.0, 40.0)])]));
    v.push(Path::new(vec![Subpath::polygon(&[(5.0, 5.0), (f64::INFINITY, 9.0), (40.0, 30.0), (10.0, f64::NEG_INFINITY)])]));
    v.push(shapes::ellipse(20.0, 20.0, 0.001, 0.002));
    v.push(shapes::ellipse(-1e7, -1e7, 2e7 + 60.0, 2e7 + 60.0));
    v.push(shapes::line((3.0, 4.0), (90.0, 70.0), 6.5));
    v.push(shapes::polygon(0.0, 0.0, 100.0, 100.0, 100, 0.9));
    v.push(Path::new(vec![Subpath::polyline(&[(10.0, 10.0), (60.0, 10.0), (60.0, 60.0), (10.0, 60.0)])]));
    // Collinear control points, cusp, loop.
    let mut c = Subpath::polyline(&[(0.0, 0.0), (80.0, 0.0)]);
    c.knots[0].out_ctrl = Point::new(30.0, 0.0);
    c.knots[1].in_ctrl = Point::new(60.0, 0.0);
    let mut l = Subpath::polygon(&[(10.0, 50.0), (90.0, 50.0)]);
    l.knots[0].out_ctrl = Point::new(120.0, -20.0);
    l.knots[1].in_ctrl = Point::new(-20.0, -20.0);
    l.knots[1].out_ctrl = Point::new(50.0, 120.0);
    v.push(Path::new(vec![c, l]));
    let mut rng = Rng(0x5eed_0001);
    for i in 0..10 {
        v.push(rand_path(&mut rng, 1 + i % 4, 3 + i % 6, 100.0));
    }
    v
}

fn styles() -> Vec<StrokeStyle> {
    let widths = [0.5, 1.0, 3.0, 10.0, 40.0];
    let limits = [1.0, 4.0, 100.0];
    let dashes: [&[f64]; 5] = [&[], &[8.0, 4.0], &[0.0, 12.0], &[3.0, 1.0, 2.0], &[5.0, -1.0, 0.0, 7.0]];
    let offsets = [0.0, 5.5, -3.0];
    let mut rng = Rng(0x57_0e5);
    let mut v = Vec::new();
    for i in 0..30 {
        v.push(StrokeStyle {
            width: widths[i % 5],
            cap: CAPS[(i / 5) % 3],
            join: JOINS[rng.below(3) as usize],
            miter_limit: limits[rng.below(3) as usize],
            dashes: dashes[rng.below(5) as usize].to_vec(),
            dash_offset: offsets[rng.below(3) as usize],
        });
    }
    v
}

fn flatten_vectors(paths: &[Path]) {
    for (i, p) in paths.iter().enumerate() {
        for (ti, tol) in [1.0, 0.25, 0.01, 0.001, 0.0, f64::NAN].iter().enumerate() {
            let lines = flatten_path(p, *tol);
            let mut h = H::new();
            let n = hash_lines(&mut h, &lines);
            println!("flatten {i} {ti} {n} {:016x}", h.0);
        }
    }
}

fn fill_vectors(paths: &[Path]) {
    for (i, p) in paths.iter().enumerate() {
        let r = fill_rasterizer(p, DEFAULT_TOLERANCE);
        let a = r.render(Rect::new(-5, -5, 125, 125));
        let b = r.render(Rect::new(13, 29, 50, 52));
        let c = r.render(Rect::new(40, 40, 41, 41));
        println!("fill {i} {} {:016x} {:016x} {:016x}", bounds_str(r.pixel_bounds()), cov_hash(&a), cov_hash(&b), cov_hash(&c));
    }
    // Large renders take the parallel strip path.
    let mut rng = Rng(0xb16);
    for i in 0..3 {
        let p = rand_path(&mut rng, 2 + i, 7, 400.0);
        let r = fill_rasterizer(&p, DEFAULT_TOLERANCE);
        let v = r.render(Rect::new(-3, -2, 397, 405));
        println!("fillbig {i} {:016x}", cov_hash(&v));
    }
    let mut r = Rasterizer::new(false);
    let max = f64::from(i32::MAX);
    r.add_component(&[vec![(max - 2.0, max - 2.0), (max + 2.0, max - 2.0), (max + 2.0, max + 2.0), (max - 2.0, max + 2.0)]], PathOp::Combine, FillRule::NonZero);
    println!("bounds max {}", bounds_str(r.pixel_bounds()));
    let mut r = Rasterizer::new(false);
    r.add_component(&[vec![(-1e12, 3.0), (1e12, 3.5), (0.0, 1e12)]], PathOp::Combine, FillRule::NonZero);
    println!("bounds huge {} {:016x}", bounds_str(r.pixel_bounds()), cov_hash(&r.render(Rect::new(-10, 0, 30, 20))));
    // Hand-built components with every op and both rules.
    let mut rng = Rng(0xc0c0);
    for i in 0..12 {
        let mut r = Rasterizer::new(i % 5 == 4);
        for c in 0..(1 + i % 4) {
            let mut polys = Vec::new();
            for _ in 0..(1 + rng.below(3)) {
                let n = 3 + rng.below(5) as usize;
                polys.push((0..n).map(|_| (rng.f() * 60.0 - 5.0, rng.f() * 60.0 - 5.0)).collect::<Vec<_>>());
            }
            let op = op_of(rng.below(5));
            let rule = if rng.below(2) == 0 { FillRule::NonZero } else { FillRule::EvenOdd };
            let _ = c;
            r.add_component(&polys, op, rule);
        }
        println!("comp {i} {:016x}", cov_hash(&r.render(Rect::new(-7, -7, 60, 60))));
    }
}

fn stroke_vectors(paths: &[Path]) {
    let sts = styles();
    for (pi, p) in paths.iter().enumerate() {
        if pi % 2 == 1 && pi > 12 {
            continue;
        }
        for (si, st) in sts.iter().enumerate() {
            if (pi + si) % 3 != 0 {
                continue;
            }
            let lines = flatten_path(p, 0.05);
            let polys = stroke_polygons(&lines, st, 0.01);
            let mut h = H::new();
            let n = hash_polys(&mut h, &polys);
            let r = stroke_rasterizer(p, st, DEFAULT_TOLERANCE);
            let c = r.render(Rect::new(-25, -25, 135, 135));
            println!("stroke {pi} {si} {n} {:016x} {} {:016x}", h.0, bounds_str(r.pixel_bounds()), cov_hash(&c));
        }
    }
    // Dashes directly.
    let pls = [
        Polyline { pts: vec![(0.0, 0.0), (10.0, 0.0)], knot: vec![true, true], closed: false },
        Polyline { pts: vec![(0.0, 0.0), (10.0, 0.0), (10.0, 7.0), (3.0, 9.0)], knot: vec![true, false, true, false], closed: true },
        Polyline { pts: vec![(1.0, 1.0)], knot: vec![true], closed: false },
        Polyline { pts: vec![(0.0, 0.0), (0.0, 0.0), (5.0, 5.0)], knot: vec![true, true, true], closed: false },
    ];
    let pats: [&[f64]; 7] = [&[2.0, 1.0], &[2.0], &[0.0, 3.0], &[1.0, 0.0], &[0.0, 0.0], &[f64::NAN, 2.0], &[0.3, 0.2, 0.1]];
    for (li, pl) in pls.iter().enumerate() {
        for (ki, pat) in pats.iter().enumerate() {
            for (oi, off) in [0.0, 1.0, -2.5, 1e9, f64::NAN, f64::INFINITY].iter().enumerate() {
                let d = dash_polyline(pl, pat, *off);
                let mut h = H::new();
                let n = hash_lines(&mut h, &d);
                println!("dash {li} {ki} {oi} {n} {:016x}", h.0);
            }
        }
    }
    // stroke_style from model strokes.
    for (i, (w, m, ds, off)) in [(4.0f32, 4.0f32, vec![2.0f32, 1.0], 0.5f32), (-1.0, 0.2, vec![], 0.0), (f32::NAN, f32::NAN, vec![f32::NAN], 3.0), (0.1, 100.0, vec![0.0, 3.0, 1.5], -2.0)].into_iter().enumerate() {
        let s = stroke_style(&ShapeStroke { width: w, miter_limit: m, dashes: ds, dash_offset: off, ..Default::default() });
        let mut h = H::new();
        h.f64(s.width);
        h.f64(s.miter_limit);
        h.f64(s.dash_offset);
        h.u64(s.dashes.len() as u64);
        for d in &s.dashes {
            h.f64(*d);
        }
        println!("style {i} {:016x}", h.0);
    }
}

fn shape_vectors() {
    let radii = [[10.0; 4], [0.0; 4], [10.0, 0.0, 10.0, 0.0], [100.0, 5.0, 0.0, 30.0], [-3.0, f64::NAN, 2.0, 1e9], [25.0, 25.0, 25.0, 25.0], [0.0, 0.0, 0.0, 1e-13]];
    let rects = [[0.0, 0.0, 100.0, 50.0], [5.5, -3.25, -40.0, 20.0], [0.0, 0.0, 0.0, 10.0], [1.0, 2.0, 50.0, 50.0]];
    for (ri, r) in rects.iter().enumerate() {
        println!("rect {ri} {:016x}", path_hash(&shapes::rect(r[0], r[1], r[2], r[3])));
        println!("ellipse {ri} {:016x}", path_hash(&shapes::ellipse(r[0], r[1], r[2], r[3])));
        for (ki, k) in radii.iter().enumerate() {
            println!("rrect {ri} {ki} {:016x}", path_hash(&shapes::rounded_rect(r[0], r[1], r[2], r[3], *k)));
            let c = shapes::clamp_radii(r[2], r[3], *k);
            let mut h = H::new();
            for x in c {
                h.f64(x);
            }
            println!("clamp {ri} {ki} {:016x}", h.0);
        }
        for sides in [0u32, 3, 5, 6, 100, 1000] {
            for (si, star) in [1.0, 0.5, 0.0, -1.0, 0.999, f64::NAN].iter().enumerate() {
                println!("polygon {ri} {sides} {si} {:016x}", path_hash(&shapes::polygon(r[0], r[1], r[2], r[3], sides, *star)));
            }
        }
    }
    for (i, (a, b, w)) in [((3.0, 4.0), (90.0, 70.0), 6.5), ((5.0, 5.0), (5.0, 5.0), 2.0), ((0.0, 0.0), (10.0, 0.0), -3.0), ((0.0, 0.0), (0.0, 10.0), f64::NAN)].iter().enumerate() {
        println!("line {i} {:016x}", path_hash(&shapes::line(*a, *b, *w)));
    }
    let lives = [
        LiveShape::Rect { rect: [1.0, 2.0, 30.0, 40.0], radii: [3.0, 4.0, 5.0, 6.0] },
        LiveShape::Ellipse { rect: [1.0, 2.0, 30.0, 40.0] },
        LiveShape::Polygon { rect: [1.0, 2.0, 30.0, 40.0], sides: 5, star_ratio: 0.4 },
        LiveShape::Line { from: [1.0, 2.0], to: [30.0, 40.0], weight: 3.0 },
    ];
    for (i, l) in lives.iter().enumerate() {
        println!("live {i} {:016x}", path_hash(&shapes::live_path(l)));
    }
}

fn hit_str(h: Option<Hit>) -> String {
    match h {
        None => "none".into(),
        Some(Hit::Anchor([s, k])) => format!("a{s},{k}"),
        Some(Hit::Handle([s, k], hd)) => format!("h{s},{k},{}", hd.name()),
        Some(Hit::Segment([s, k], t)) => format!("s{s},{k},{:016x}", t.to_bits()),
    }
}

fn res_str(r: Result<(), String>, p: &Path) -> String {
    match r {
        Ok(()) => format!("{:016x}", path_hash(p)),
        Err(e) => format!("ERR {e}"),
    }
}

fn edit_vectors(paths: &[Path]) {
    let ts = [0.0, 0.05, 0.3, 0.5, 0.95, 1.0, f64::NAN, f64::INFINITY, -2.0];
    for (pi, base) in [curve(), tri(), paths[2].clone(), paths[1].clone(), paths[25].clone(), paths[30].clone()].iter().enumerate() {
        let mut p = base.clone();
        let r = edit::move_anchors(&mut p, &[[0, 1], [0, 1], [0, 0]], [5.0, -2.0]);
        println!("move {pi} {}", res_str(r, &p));
        let mut p = base.clone();
        let r = edit::move_anchors(&mut p, &[[0, 0], [0, 99]], [1.0, 1.0]);
        println!("move {pi} bad {}", res_str(r, &p));
        for (hi, h) in [Handle::In, Handle::Out].iter().enumerate() {
            for ind in [false, true] {
                let mut p = base.clone();
                let r = edit::move_handle(&mut p, [0, 1], *h, Point::new(50.0, 30.0), ind);
                println!("handle {pi} {hi} {} {}", ind as u8, res_str(r, &p));
            }
        }
        let mut p = base.clone();
        let r = edit::convert_point(&mut p, [0, 1], None);
        println!("convert {pi} none {}", res_str(r, &p));
        let mut p = base.clone();
        let r = edit::convert_point(&mut p, [0, 0], Some(Point::new(10.0, 60.0)));
        println!("convert {pi} some {}", res_str(r, &p));
        let mut p = base.clone();
        let r = edit::convert_point(&mut p, [usize::MAX, usize::MAX], None);
        println!("convert {pi} bad {}", res_str(r, &p));
        let nk = base.subpaths.first().map_or(0, |s| s.knots.len());
        for k in [0, 1, nk.saturating_sub(1), nk, usize::MAX] {
            for (ti, t) in ts.iter().enumerate() {
                let mut p = base.clone();
                let r = edit::bend_segment(&mut p, [0, k], *t, [3.0, 12.0]);
                println!("bend {pi} {k} {ti} {}", res_str(r, &p));
            }
        }
        let mut p = base.clone();
        let r = edit::bend_segment(&mut p, [7, 0], 0.5, [3.0, 12.0]);
        println!("bend {pi} badsub {}", res_str(r, &p));
        for (bi, bx) in [[110.0, -5.0, 40.0, 5.0], [0.0, 0.0, 100.0, 100.0], [f64::NAN, 0.0, 100.0, 100.0], [-1e9, -1e9, 1e9, 1e9]].iter().enumerate() {
            let v = edit::anchors_in(base, *bx);
            let s: Vec<String> = v.iter().map(|[a, b]| format!("{a}.{b}")).collect();
            println!("box {pi} {bi} {}", s.join(","));
        }
        for s in [0, 1, 4] {
            let v = edit::subpath_anchors(base, s);
            println!("subanchors {pi} {s} {}", v.len());
        }
        // Hit tests over a grid, with all handles of knot 1 shown (and a bogus one).
        let shown = [([0, 1], Handle::Out), ([0, 1], Handle::In), ([9, 9], Handle::In)];
        let mut h = H::new();
        for gy in 0..12 {
            for gx in 0..12 {
                let pt = Point::new(gx as f64 * 9.0 - 4.0, gy as f64 * 9.0 - 4.0);
                for (tol, hs) in [(3.0, &shown[..]), (8.0, &[][..]), (0.0, &shown[..])] {
                    h.b(hit_str(edit::hit(base, pt, tol, hs)).as_bytes());
                }
            }
        }
        println!("hit {pi} {:016x}", h.0);
        println!("hit1 {pi} {}", hit_str(edit::hit(base, Point::new(71.0, 1.0), 3.0, &shown)));
        println!("hit2 {pi} {}", hit_str(edit::hit(base, Point::new(50.0, 1.0), 3.0, &[])));
        println!("hit3 {pi} {}", hit_str(edit::hit(base, Point::new(f64::NAN, 1.0), 3.0, &shown)));
    }
    for s in ["in", "out", "In", "", "outer"] {
        println!("handle_parse {s:?} {}", Handle::parse(s).map_or("none", |h| h.name()));
    }
}

fn masks() -> Vec<(Rect, Vec<f32>, f32)> {
    let mut out = Vec::new();
    let rect = Rect::new(0, 0, 160, 120);
    let mut src = shapes::ellipse(10.0, 10.0, 90.0, 90.0);
    src.subpaths.push(shapes::ellipse(35.0, 35.0, 40.0, 40.0).subpaths.remove(0).with_op(PathOp::Subtract));
    src.subpaths.push(shapes::rect(110.0, 20.0, 40.0, 80.0).subpaths.remove(0));
    let mask = path_coverage(&src, rect);
    let hard: Vec<f32> = mask.iter().map(|v| if *v >= 0.5 { 1.0 } else { 0.0 }).collect();
    out.push((rect, mask, 0.0));
    out.push((rect, hard, 0.0));
    let r2 = Rect::new(-7, 3, 33, 33);
    let mut rng = Rng(0x7ace);
    let noise: Vec<f32> = (0..40 * 30).map(|_| rng.f() as f32).collect();
    out.push((r2, noise.clone(), 0.0));
    out.push((r2, noise, 1.0));
    let blocky: Vec<f32> = (0..40 * 30).map(|i| if (i % 40) / 5 % 2 == (i / 40) / 4 % 2 { 1.0 } else { 0.0 }).collect();
    out.push((r2, blocky, 0.0));
    let mut withnan: Vec<f32> = (0..20 * 20).map(|i| if (i % 20 - 10i32).abs() + (i / 20 - 10i32).abs() < 7 { 1.0 } else { 0.0 }).collect();
    withnan[5 * 20 + 10] = f32::NAN;
    withnan[10 * 20 + 10] = f32::INFINITY;
    out.push((Rect::new(0, 0, 20, 20), withnan, f32::NAN));
    out.push((Rect::new(0, 0, 1, 1), vec![1.0], 0.0));
    out.push((Rect::new(0, 0, 3, 1), vec![0.6, 0.4, 0.6], 0.5));
    let sad: Vec<f32> = vec![1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0];
    out.push((Rect::new(0, 0, 3, 3), sad, 0.0));
    out
}

fn trace_vectors() {
    for (mi, (rect, v, outside)) in masks().iter().enumerate() {
        let cs = trace::contours(v, *rect, *outside);
        let mut h = H::new();
        let n = hash_polys(&mut h, &cs);
        println!("contours {mi} {n} {:016x}", h.0);
        for (ti, tol) in [0.5, 1.0, 2.0, 0.0, f64::NAN].iter().enumerate() {
            let p = trace::trace_mask(v, *rect, *outside, *tol);
            let knots: usize = p.subpaths.iter().map(|s| s.knots.len()).sum();
            println!("trace {mi} {ti} {knots} {:016x}", path_hash(&p));
        }
    }
}

fn shape_layer(path: Path, fill: Option<Fill>, stroke: Option<ShapeStroke>) -> ShapeLayer {
    ShapeLayer { path, fill, stroke, ..Default::default() }
}

fn rgba_hash(v: &[[f32; 4]]) -> u64 {
    let mut h = H::new();
    for p in v {
        for c in p {
            h.f32(*c);
        }
    }
    h.0
}

fn paint_vectors() {
    let mut rng = Rng(0x9a17);
    let colors = [Color::BLACK, Color::WHITE, Color::rgb(1.0, 0.0, 0.0), Color::rgba(0.2, 0.5, 0.9, 0.5), Color::gray(0.3)];
    for i in 0..40 {
        let nst = 1 + rng.below(4) as usize;
        let stops: Vec<(f32, Color)> = (0..nst).map(|_| (rng.f() as f32 * 1.2 - 0.1, colors[rng.below(5) as usize])).collect();
        let nop = rng.below(3) as usize;
        let opacity_stops: Vec<(f32, f32)> = (0..nop).map(|_| (rng.f() as f32, rng.f() as f32)).collect();
        let nmid = rng.below(3) as usize;
        let midpoints: Vec<f32> = (0..nmid).map(|_| rng.f() as f32).collect();
        let angles = [0.0f32, 30.0, 90.0, 135.0, -45.0, 400.0, 180.0, 1e-3];
        let fill = Fill::Gradient {
            stops,
            angle: angles[rng.below(8) as usize],
            scale: [0.5f32, 1.0, 1.3, 0.0][rng.below(4) as usize],
            style: STYLES[i % 5],
            reverse: rng.below(2) == 0,
            opacity_stops,
            midpoints,
            offset: ([0.0f32, 0.1, -0.3][rng.below(3) as usize], [0.0f32, -0.1, 0.25][rng.below(3) as usize]),
            dither: rng.below(3) == 0,
            align: rng.below(3) != 0,
        };
        let path = match i % 3 {
            0 => shapes::rect(0.0, 0.0, 100.0, 10.0),
            1 => shapes::ellipse(3.5, 2.25, 40.0, 33.0),
            _ => shapes::polygon(1.0, 1.0, 37.0, 29.0, 5, 0.5),
        };
        let stroke = if i % 4 == 3 {
            Some(ShapeStroke { width: 3.0, paint: Fill::Solid(Color::rgba(0.0, 0.0, 1.0, 0.7)), opacity: 0.8, align: [StrokeAlign::Inside, StrokeAlign::Center, StrokeAlign::Outside][i % 3], ..Default::default() })
        } else if i % 4 == 1 {
            Some(ShapeStroke { width: 2.0, paint: fill.clone(), ..Default::default() })
        } else {
            None
        };
        let sh = shape_layer(path, Some(fill), stroke);
        let canvas = Rect::new(0, 0, 120, 50);
        let cs = CompiledShape::new(&sh, DEFAULT_TOLERANCE, canvas);
        let px = cs.render_rgba(Rect::new(-2, -3, 110, 45));
        println!("paint {i} {} {:016x}", bounds_str(cs.bounds()), rgba_hash(&px));
    }
    // Strokes: alignment, opacity, dashes, open subpaths.
    let sq = shapes::rect(20.0, 20.0, 60.0, 40.0);
    let mut open = sq.clone();
    open.subpaths[0].closed = false;
    for (i, path) in [sq, open, curve(), pentagram()].iter().enumerate() {
        for (ai, align) in [StrokeAlign::Inside, StrokeAlign::Center, StrokeAlign::Outside].iter().enumerate() {
            for (fi, fill) in [None, Some(Fill::Solid(Color::rgb(1.0, 0.0, 0.0)))].iter().enumerate() {
                let st = ShapeStroke { width: 6.0, paint: Fill::Solid(Color::rgba(0.0, 0.5, 1.0, 0.9)), opacity: 0.75, align: *align, cap: CAPS[ai], join: JOINS[fi + ai % 2], dashes: if i == 3 { vec![2.0, 1.0] } else { vec![] }, ..Default::default() };
                let sh = shape_layer(path.clone(), fill.clone(), Some(st));
                let cs = CompiledShape::new(&sh, DEFAULT_TOLERANCE, Rect::new(0, 0, 100, 100));
                let px = cs.render_rgba(Rect::new(0, 0, 100, 100));
                println!("strokeshape {i} {ai} {fi} {} {:016x}", bounds_str(cs.bounds()), rgba_hash(&px));
            }
        }
    }
    // Zero width / zero opacity strokes, pattern paint, inverted fill.
    let mut inv = shapes::ellipse(5.0, 5.0, 20.0, 20.0);
    inv.inverted = true;
    let cases = [
        shape_layer(shapes::rect(2.0, 2.0, 10.0, 10.0), None, Some(ShapeStroke { width: 0.0, ..Default::default() })),
        shape_layer(shapes::rect(2.0, 2.0, 10.0, 10.0), None, Some(ShapeStroke { opacity: 0.0, ..Default::default() })),
        shape_layer(shapes::rect(2.0, 2.0, 10.0, 10.0), Some(Fill::Pattern { name: "p".into(), scale: 1.0, id: "x".into(), angle: 0.0, link: true, phase: (0.0, 0.0) }), None),
        shape_layer(inv, Some(Fill::Solid(Color::rgb(0.0, 1.0, 0.0))), None),
        shape_layer(Path::default(), Some(Fill::Solid(Color::BLACK)), Some(ShapeStroke::default())),
    ];
    for (i, sh) in cases.iter().enumerate() {
        let cs = CompiledShape::new(sh, DEFAULT_TOLERANCE, Rect::new(0, 0, 30, 30));
        println!("shapecase {i} {} {:016x}", bounds_str(cs.bounds()), rgba_hash(&cs.render_rgba(Rect::new(0, 0, 30, 30))));
    }
}

fn surface_hash(s: &photocraft_raster::Surface, r: Rect) -> String {
    let b = s.to_interleaved(r);
    let mut h = H::new();
    h.b(&b);
    let cb = s.content_bounds();
    format!("{},{},{},{} {} {:016x}", cb.x0, cb.y0, cb.x1, cb.y1, s.tile_count(), h.0)
}

fn surface_vectors() {
    let sh = ShapeLayer {
        path: {
            let mut p = shapes::ellipse(-30.0, 40.0, 600.0, 300.0);
            p.subpaths.push(shapes::rect(100.0, 100.0, 50.0, 400.0).subpaths.remove(0).with_op(PathOp::Exclude));
            p
        },
        fill: Some(Fill::gradient(vec![(0.0, Color::rgb(1.0, 0.0, 0.0)), (1.0, Color::rgba(0.0, 0.0, 1.0, 0.5))], 30.0, 1.0, GradientStyle::Linear, false)),
        stroke: Some(ShapeStroke { width: 5.0, paint: Fill::Solid(Color::rgb(0.1, 0.9, 0.2)), dashes: vec![3.0, 2.0], join: LineJoin::Round, ..Default::default() }),
        ..Default::default()
    };
    let clip = Rect::new(0, 0, 640, 520);
    let fmts = [
        PixelFormat::RGBA8,
        PixelFormat::RGBA16,
        PixelFormat::new(ColorMode::Rgb, SampleType::F32, true),
        PixelFormat::GRAY8,
        PixelFormat::new(ColorMode::Grayscale, SampleType::U8, true),
        PixelFormat::new(ColorMode::Cmyk, SampleType::U8, true),
        PixelFormat::new(ColorMode::Rgb, SampleType::U16, false),
    ];
    for (i, f) in fmts.iter().enumerate() {
        let s = render_shape(&sh, *f, clip);
        println!("surface {i} {}", surface_hash(&s, clip));
    }
    let small = ShapeLayer {
        path: shapes::rect(2.0, 2.0, 10.0, 10.0),
        fill: Some(Fill::Solid(Color::rgb(1.0, 0.0, 0.0))),
        stroke: Some(ShapeStroke { width: 2.0, paint: Fill::Solid(Color::rgb(0.0, 0.0, 1.0)), ..Default::default() }),
        ..Default::default()
    };
    for (i, c) in [Rect::new(0, 0, 20, 20), Rect::new(0, 0, 5, 5), Rect::new(-300, -300, -100, -100), Rect::new(5, 5, 5, 9)].iter().enumerate() {
        let s = render_shape(&small, PixelFormat::RGBA8, *c);
        println!("smallsurface {i} {}", surface_hash(&s, Rect::new(-1, -1, 21, 21)));
    }
    // Coverage surfaces.
    let p = shapes::rect(10.0, 10.0, 20.0, 20.0);
    let r = fill_rasterizer(&p, DEFAULT_TOLERANCE);
    for (i, sample) in SampleType::ALL.iter().enumerate() {
        let s = coverage_surface(&r, PixelFormat::new(ColorMode::Rgb, *sample, true), Rect::new(0, 0, 300, 300));
        println!("coverage {i} {}", surface_hash(&s, Rect::new(0, 0, 300, 300)));
    }
    let mut inv = shapes::ellipse(100.0, 50.0, 300.0, 200.0);
    inv.inverted = true;
    let s = coverage_surface(&fill_rasterizer(&inv, DEFAULT_TOLERANCE), PixelFormat::GRAY8, Rect::new(-20, -10, 530, 290));
    println!("coverage inv {}", surface_hash(&s, Rect::new(-20, -10, 530, 290)));
    // Vector masks.
    for (i, (density, enabled, inverted, empty)) in [(1.0f32, true, false, false), (0.25, true, false, false), (0.25, false, false, false), (1.0, true, true, true), (0.5, true, false, true), (f32::NAN, true, true, false), (-0.5, true, false, false)].iter().enumerate() {
        let mut path = if *empty { Path::default() } else { shapes::ellipse(3.0, 4.0, 30.0, 20.0) };
        path.inverted = *inverted;
        let mut vm = VectorMask::new(path);
        vm.density = *density;
        vm.enabled = *enabled;
        let v = vector_mask_values(&vm, Rect::new(0, 0, 40, 30));
        println!("mask {i} {:016x}", cov_hash(&v));
    }
}

pub fn run() {
    let paths = corpus();
    for (i, p) in paths.iter().enumerate() {
        println!("path {i} {:016x}", path_hash(p));
    }
    flatten_vectors(&paths);
    fill_vectors(&paths);
    stroke_vectors(&paths);
    shape_vectors();
    edit_vectors(&paths);
    trace_vectors();
    paint_vectors();
    surface_vectors();
}
