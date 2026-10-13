// Included by paint_vectors.rs (resync chunk R7): vectors for what crates/paint gained after
// efa62efa. Line kinds (appended after the original ones):
//
//   falloff HASH                       tip_falloff / half_coverage_radius sweep
//   texlevel HASH                      Texture::level sweep
//   texmode MODE HASH                  texture_combine sweep
//   preview W H SEED MAX RESULT        GrayTile::preview ("w,h:hash"); W = "bad" for invalid tiles
//   outline BRUSH OVR FULL RESULT          outline::tip_outline ("none" or "rings:lens:hash")
//   sym MODE CX CY ROT RESULT          PresetSymmetry (new, directions, segments, reflected passes)
//   smoothx CFG ZOOM PTS RESULT        Smoother point by point: outputs, lagging, string, finish
//   tdabs BRUSH PTS RESULT             tail_dabs / wants_time / smoothing_string after pushing
//   ulive BRUSH SCEN NPASS CHUNK RES   composite_union_live per chunk (damage, live bounds, hash),
//                                      then composite_union_many of the finished passes
//   (plus dabs / stroke / rast / ser / de lines for the new brushes and serde forms)
use photocraft_paint::dynamics::Smoother;
use photocraft_paint::outline::tip_outline;
use photocraft_paint::render::{texture_combine, UnionLive};
use photocraft_paint::symmetry::{PresetSymmetry, SymmetryMode};
use photocraft_paint::tile::StoredTile;
use photocraft_paint::{half_coverage_radius, tip_falloff};

const HOSTILE: [f32; 6] = [f32::NAN, f32::INFINITY, f32::NEG_INFINITY, -0.0, 1e-30, 3.0e38];

fn r7_sweep() -> Vec<f32> {
    let mut v: Vec<f32> = (0..=40).map(|i| i as f32 / 40.0).collect();
    v.extend([-0.5, 1.5, 0.999, 0.001, 0.4999, 0.5001]);
    v.extend(HOSTILE);
    v
}

fn r7_stored(seed: u64, loaded: bool) -> StoredTile {
    let full = gtile(40, 24, seed);
    StoredTile {
        key: format!("k{seed}"),
        width: 40,
        height: 24,
        preview: std::sync::Arc::new(full.preview(8)),
        full: loaded.then(|| std::sync::Arc::new(full)),
    }
}

fn r7_brushes() -> Vec<BrushSettings> {
    let b = base;
    let mut v = Vec::new();
    // Pencils: pixel-grid stepping (size * spacing <= 1) and not.
    for (size, spacing) in [(1.0, 0.25), (2.0, 0.25), (1.0, 1.0), (3.0, 0.3), (1.0, 2.0), (2.0, 0.5)] {
        v.push(BrushSettings { aliased: true, hardness: 1.0, size, spacing, ..b() });
    }
    v.push(BrushSettings { aliased: true, size: 1.0, spacing_enabled: false, ..b() });
    // Block (square) erasers and the square flag without aliasing.
    v.push(BrushSettings { aliased: true, square: true, size: 7.0, erase: true, ..b() });
    v.push(BrushSettings { aliased: true, square: true, size: 6.0, angle: 30.0, roundness: 0.4, ..b() });
    v.push(BrushSettings { square: true, size: 6.0, ..b() });
    // Small computed tips (area coverage below 3 px radius).
    for (size, hardness) in [(1.0, 1.0), (2.5, 0.0), (4.0, 0.5), (5.9, 1.0), (6.1, 0.3)] {
        v.push(BrushSettings { size, hardness, roundness: 0.7, angle: 20.0, ..b() });
    }
    // Stroke-level Wet Edges.
    v.push(BrushSettings { size: 30.0, hardness: 0.8, wet_edges: true, ..b() });
    v.push(BrushSettings { size: 22.0, hardness: 0.2, wet_edges: true, noise: true, flow: 0.4, ..b() });
    // The new Texture modes, levels and stored patterns.
    for mode in [MaskMode::Height, MaskMode::LinearHeight, MaskMode::Subtract, MaskMode::ColorBurn, MaskMode::ColorDodge] {
        for each_tip in [false, true] {
            v.push(BrushSettings {
                size: 28.0,
                texture: Texture {
                    enabled: true,
                    pattern: Pattern::Tile(gtile(9, 7, 6)),
                    mode,
                    depth: 0.13,
                    brightness: -87.0 / 150.0,
                    contrast: 0.3,
                    each_tip,
                    ..Default::default()
                },
                ..b()
            });
        }
    }
    v.push(BrushSettings {
        size: 28.0,
        texture: Texture { enabled: true, pattern: Pattern::Stored(r7_stored(8, false)), contrast: -0.6, invert: true, ..Default::default() },
        ..b()
    });
    // Stored tips (unloaded: they paint with the preview, and serialise as stored).
    v.push(BrushSettings { tip: TipShape::Stored(r7_stored(9, false)), size: 25.0, angle: 40.0, ..b() });
    v.push(BrushSettings { tip: TipShape::Stored(r7_stored(10, false)), size: 12.0, aliased: true, ..b() });
    v
}

fn ring_h(rings: &Option<Vec<Vec<[f32; 2]>>>) -> String {
    match rings {
        None => "none".into(),
        Some(r) => {
            let mut h = H0;
            for ring in r {
                u64le(&mut h, ring.len() as u64);
                for p in ring {
                    f32b(&mut h, p[0]);
                    f32b(&mut h, p[1]);
                }
            }
            let lens: Vec<String> = r.iter().map(|x| x.len().to_string()).collect();
            format!("{}:{}:{h:016x}", r.len(), lens.join(","))
        }
    }
}

fn pts_h(h: &mut u64, pts: &[StrokePoint]) {
    for p in pts {
        for v in [p.x, p.y, p.time] {
            f64b(h, v);
        }
        for v in [p.pressure, p.tilt_x, p.tilt_y, p.rotation, p.wheel] {
            f32b(h, v);
        }
    }
}

fn smoothx_line(cfg: &Smoothing, zoom: f32, pts: &[StrokePoint]) {
    let mut s = Smoother::new(cfg, zoom);
    let mut h = H0;
    let mut n = 0usize;
    let mut flags = String::new();
    for p in pts {
        let mut out = Vec::new();
        s.push(*p, &mut out);
        n += out.len();
        pts_h(&mut h, &out);
        flags.push(if s.lagging() { 'L' } else { '-' });
        match s.string() {
            Some((a, b)) => {
                flags.push('S');
                for v in a.iter().chain(b.iter()) {
                    f64b(&mut h, *v);
                }
            }
            None => flags.push('-'),
        }
    }
    let mut out = Vec::new();
    s.finish(&mut out);
    n += out.len();
    pts_h(&mut h, &out);
    let c = format!(
        "{},{},{},{},{}",
        x32(cfg.amount),
        cfg.pulled_string as u8,
        cfg.catch_up as u8,
        cfg.catch_up_on_end as u8,
        cfg.adjust_for_zoom as u8
    );
    let flags = if flags.is_empty() { "-".to_string() } else { flags };
    l!("smoothx", c, x32(zoom), pts_s(pts), format!("{n}:{flags}:{h:016x}"));
}

fn tdabs_line(b: &BrushSettings, pts: &[StrokePoint]) {
    let mut r = StrokeRenderer::new(b, Some(PixelFormat::RGBA8), 1.0);
    let mut out = Vec::new();
    for k in 0..pts.len() {
        r.push(&pts[k..k + 1]);
        let t = r.tail_dabs();
        let mut h = H0;
        for d in &t {
            dab_h(&mut h, d);
        }
        let s = match r.smoothing_string() {
            Some((a, c)) => format!("{},{},{},{}", x64(a[0]), x64(a[1]), x64(c[0]), x64(c[1])),
            None => "-".into(),
        };
        out.push(format!("{}:{}:{h:016x}:{s}", t.len(), r.wants_time() as u8));
    }
    l!("tdabs", bj(b), pts_s(pts), out.join(" "));
}

fn ulive_line(b: &BrushSettings, scen: usize, npass: usize, chunk: usize, pts: &[StrokePoint]) {
    let (s0, sel, lock) = scenario(scen);
    let fmt = s0.format();
    let pre = s0.clone();
    let mut target = s0.clone();
    let mirror = |fx: bool, fy: bool| -> Vec<StrokePoint> {
        pts.iter().map(|p| StrokePoint { x: if fx { 200.0 - p.x } else { p.x }, y: if fy { 150.0 - p.y } else { p.y }, ..*p }).collect()
    };
    let paths = [pts.to_vec(), mirror(true, false), mirror(false, true), mirror(true, true)];
    let mut rs: Vec<StrokeRenderer> = (0..4).map(|_| StrokeRenderer::new(b, Some(fmt), 1.0)).collect();
    let mut live = UnionLive::default();
    let mut out = Vec::new();
    let (mut at, mut merged) = (0usize, 0usize);
    while at < pts.len() {
        let end = (at + chunk).min(pts.len());
        for (r, p) in rs.iter_mut().zip(&paths) {
            r.push(&p[at..end]);
        }
        // Passes join part-way, as when symmetry is switched on mid-stroke.
        let count = if at * 2 >= pts.len() { npass } else { npass.min(2) };
        let full = merged != count;
        merged = count;
        let mut passes: Vec<&mut StrokeRenderer> = rs.iter_mut().take(count).collect();
        let d = StrokeRenderer::composite_union_live(&mut passes, &mut live, full, &pre, &mut target, sel.as_ref(), lock);
        out.push(format!("{}|{}|{:016x}", rect_s(d), rect_s(live.bounds()), surf_h(&target)));
        at = end;
    }
    let fin: Vec<StrokeRenderer> = rs
        .iter()
        .take(merged.max(1))
        .map(|r| {
            let mut r = r.clone();
            r.finish();
            r
        })
        .collect();
    let mut t2 = pre.clone();
    let d = fin[0].composite_union_many(&fin[1..], &pre, &mut t2, sel.as_ref(), lock);
    out.push(format!("M{}|{:016x}", rect_s(d), surf_h(&t2)));
    l!("ulive", bj(b), scen, npass, chunk, pts_s(pts), out.join(" "));
}

fn r7_json_cases() -> Vec<(&'static str, String)> {
    let mut v: Vec<(&'static str, String)> = Vec::new();
    for s in [
        r#"{"stored": {"key": "abc", "width": 300, "height": 200, "preview": {"width": 1, "height": 1, "data": [0.5]}}}"#,
        r#"{"stored": {"key": "abc", "width": 300, "height": 200, "preview": {"width": 1, "height": 1, "data": [0.5]}, "full": 3}}"#,
        r#"{"stored": {"key": "k", "width": 1, "height": 1}}"#,
        r#"{"stored": 5}"#,
        r#""stored""#,
        r#"{"stored": null}"#,
        r#"{"stored": {"key": 1, "width": 1, "height": 1, "preview": 0}}"#,
        r#"{"stored": ["k", 2, 3, [1, 1, [1.0]]]}"#,
        r#"{"stored": ["k", 2, 3]}"#,
        r#"{"stored": {"key": "", "width": 4294967296, "height": 1, "preview": [1, 1, [0]]}}"#,
        r#"{"stored": {"key": "x", "width": 1, "height": 1, "preview": {"width": 2, "height": 2, "data": [0]}}}"#,
        r#"{"sampled": {"width": 1, "height": 1, "data": [1]}, "stored": null}"#,
        r#"{"tile": null}"#,
    ] {
        v.push(("TipShape", s.into()));
    }
    for s in [
        r#"{"stored": {"key": "p", "width": 3, "height": 3, "preview": [1, 1, [0.25]]}}"#,
        r#"{"stored": {"key": "p"}}"#,
        r#""stored""#,
    ] {
        v.push(("Pattern", s.into()));
    }
    for s in [
        r#"{"name": "A", "brush": {}, "folder": ["x", "y"]}"#,
        r#"{"name": "A", "brush": {}, "group": "G", "folder": []}"#,
        r#"{"name": "A", "brush": {}, "folder": "x"}"#,
        r#"{"name": "A", "brush": {}, "folder": [1]}"#,
        r#"{"name": "A", "brush": {}, "folder": null}"#,
        r#"["N", {}, false, "g", ["a", "b"]]"#,
        r#"["N", {}, false, "g", ["a"], 1]"#,
    ] {
        v.push(("BrushPreset", s.into()));
    }
    for s in [
        r#"{"square": true}"#,
        r#"{"square": false, "aliased": true}"#,
        r#"{"square": 1}"#,
        r#"{"tip": {"stored": {"key": "t", "width": 64, "height": 32, "preview": [2, 1, [0.5, 1.0]]}}}"#,
        r#"{"texture": {"enabled": true, "pattern": {"stored": {"key": "t", "width": 1, "height": 1, "preview": [1, 1, [1.0]]}}}}"#,
    ] {
        v.push(("BrushSettings", s.into()));
    }
    v
}

fn r7_de_line(ty: &str, input: &str) {
    if ty != "TipShape" {
        de_line(ty, input);
        return;
    }
    let val: J = serde_json::from_str(input).unwrap();
    let r = match serde_json::from_value::<TipShape>(val) {
        Ok(x) => serde_json::to_string(&x).unwrap(),
        Err(e) => format!("ERR {}", trunc(e.to_string())),
    };
    l!("de", ty, input, r);
}

fn r7(sets: &[Vec<StrokePoint>]) {
    // ---- tip falloff ----
    let sw = r7_sweep();
    let mut h = H0;
    for &d in &sw {
        for r in [10.0f32, 0.5, 1e-4, 0.0, -2.0, 3.0e38, f32::NAN, f32::INFINITY] {
            for &hd in &sw {
                f32b(&mut h, tip_falloff(d * 12.0, r, hd));
            }
        }
    }
    for r in [10.5f32, 1.0, 0.0, -1.0, f32::NAN, f32::INFINITY] {
        for &hd in &sw {
            f32b(&mut h, half_coverage_radius(r, hd));
        }
    }
    l!("falloff", format!("{h:016x}"));
    // ---- texture levels and modes ----
    let mut h = H0;
    for &br in &sw {
        for &ct in &[-1.0f32, -0.6, -0.3, 0.0, 0.3, 0.5, 0.999, 1.0, 1.5, f32::NAN, f32::INFINITY] {
            for invert in [false, true] {
                let t = Texture { brightness: br * 2.0 - 1.0, contrast: ct, invert, ..Default::default() };
                for &v in &sw {
                    f32b(&mut h, t.level(v));
                }
            }
        }
    }
    l!("texlevel", format!("{h:016x}"));
    for m in [
        MaskMode::Multiply,
        MaskMode::Subtract,
        MaskMode::Darken,
        MaskMode::Overlay,
        MaskMode::ColorDodge,
        MaskMode::ColorBurn,
        MaskMode::LinearBurn,
        MaskMode::HardMix,
        MaskMode::LinearHeight,
        MaskMode::Height,
    ] {
        let mut h = H0;
        for &v in &sw {
            for &t in &sw {
                for &d in &[0.0f32, 0.002, 0.01, 0.05, 0.13, 0.25, 0.5, 1.0, 1.2, -0.1, f32::NAN] {
                    f32b(&mut h, texture_combine(m, v, t, d));
                }
            }
        }
        l!("texmode", serde_json::to_string(&m).unwrap().trim_matches('"'), format!("{h:016x}"));
    }
    // ---- previews ----
    for (w, hh, seed, max) in [(1000u32, 400u32, 1u64, 96u32), (37, 5, 2, 8), (5, 3, 3, 96), (96, 97, 4, 96), (300, 200, 5, 0), (1, 1, 6, 1), (64, 64, 7, 7)] {
        let p = gtile(w, hh, seed).preview(max);
        let mut h = H0;
        for x in &p.data {
            fnv(&mut h, &x.to_le_bytes());
        }
        l!("preview", w, hh, seed, max, format!("{},{}:{h:016x}", p.width, p.height));
    }
    for (i, t) in [GrayTile { width: 4, height: 4, data: vec![1] }, GrayTile { width: 0, height: 0, data: vec![] }].iter().enumerate() {
        let p = t.preview(96);
        l!("preview", "bad", t.width, i, 96, format!("{},{}:{:?}", p.width, p.height, p.data));
    }
    // ---- outlines ----
    let b = base;
    let mut obs = vec![b(), BrushSettings { size: 40.0, angle: 33.0, roundness: 0.4, hardness: 0.25, ..b() }];
    obs.push(BrushSettings { size: 3.0, angle: -170.0, roundness: 0.0, hardness: 0.0, flip_x: true, ..b() });
    obs.push(BrushSettings { size: 5000.0, roundness: 1.5, ..b() });
    obs.push(BrushSettings { tip: TipShape::Sampled(gtile(16, 12, 3)), size: 25.0, angle: 60.0, flip_x: true, ..b() });
    obs.push(BrushSettings { tip: TipShape::Sampled(gtile(9, 20, 5)), size: 14.0, roundness: 0.6, flip_y: true, ..b() });
    obs.push(BrushSettings { tip: TipShape::Sampled(procedural::chalk_tip(64, 31)), size: 50.0, angle: 15.0, ..b() });
    obs.push(BrushSettings { tip: TipShape::Sampled(procedural::spatter_tip(48, 23, 20)), size: 30.0, ..b() });
    obs.push(BrushSettings { tip: TipShape::Sampled(procedural::star_tip(64)), size: 30.0, roundness: 0.3, ..b() });
    obs.push(BrushSettings { tip: TipShape::Sampled(procedural::leaf_tip(64)), size: 30.0, angle: 90.0, ..b() });
    obs.push(BrushSettings { tip: TipShape::Sampled(gtile(400, 170, 11)), size: 80.0, ..b() });
    obs.push(BrushSettings { tip: TipShape::Sampled(GrayTile::from_fn(8, 8, |_, _| 0.0)), size: 20.0, ..b() });
    obs.push(BrushSettings { tip: TipShape::Sampled(GrayTile::from_fn(8, 8, |_, _| 1.0)), size: 20.0, ..b() });
    obs.push(BrushSettings { tip: TipShape::Stored(r7_stored(9, false)), size: 25.0, angle: 40.0, ..b() });
    obs.push(BrushSettings { size: 0.0, ..b() });
    obs.push(BrushSettings { size: -1.0, ..b() });
    // Non-finite settings travel as overrides (JSON has no NaN).
    let ovrs: Vec<Vec<(&str, f32)>> = vec![
        vec![],
        vec![("hardness", f32::NAN)],
        vec![("size", f32::NAN)],
        vec![("size", f32::INFINITY)],
        vec![("angle", f32::INFINITY)],
        vec![("roundness", f32::NAN)],
    ];
    for (i, ob) in obs.iter().enumerate() {
        for o in ovrs.iter().filter(|o| o.is_empty() || i % 4 == 1) {
            let mut bb = ob.clone();
            apply_ovr(&mut bb, o);
            for full in [false, true] {
                l!("outline", bj(ob), ovr_s(o), full as u8, ring_h(&tip_outline(&bb, full)));
            }
        }
    }
    // ---- symmetry ----
    let sym_pts = [
        StrokePoint { tilt_x: 18.0, ..pt(12.0, 25.0, 0.4) },
        pt(-3.5, 1e5, 1.0),
        StrokePoint { time: 7.0, ..pt(640.25, 0.0, 0.0) },
    ];
    for mode in SymmetryMode::ALL {
        for (c, rot) in [
            ([20.0, 30.0], 0.0),
            ([20.0, 30.0], 90.0),
            ([50.5, 26.5], -37.5),
            ([0.0, 0.0], 720.25),
            ([320.0, 240.0], 1e-12),
            ([20.0, -5.0], 0.0),
            ([1e6, -1e6], 359.999),
            ([1e6 + 1.0, 0.0], 0.0),
            ([f64::NAN, 0.0], 0.0),
            ([0.0, 0.0], f64::INFINITY),
        ] {
            let r = match PresetSymmetry::new(mode, c, rot) {
                None => "none".to_string(),
                Some(s) => {
                    let mut h = H0;
                    f64b(&mut h, s.rotation);
                    for d in s.directions() {
                        f64b(&mut h, d[0]);
                        f64b(&mut h, d[1]);
                    }
                    let mut segs = Vec::new();
                    for size in [[100.0, 100.0], [101.0, 53.0], [640.0, 480.0], [0.0, 0.0]] {
                        let sg = s.segments(size);
                        segs.push(sg.len().to_string());
                        for [a, e] in sg {
                            for v in [a[0], a[1], e[0], e[1]] {
                                f64b(&mut h, v);
                            }
                        }
                    }
                    let passes = s.reflected_passes(&sym_pts);
                    for p in &passes {
                        pts_h(&mut h, p);
                    }
                    format!("{}:{}:{}:{h:016x}", s.mirror_count(), passes.len(), segs.join(","))
                }
            };
            l!("sym", mode.id(), x64(c[0]), x64(c[1]), x64(rot), r);
        }
    }
    // ---- smoothing point by point ----
    let timed: Vec<StrokePoint> = [(0.0, 0.0, 0.0), (100.0, 0.0, 1.0), (100.0, 0.0, 17.0), (100.0, 0.0, 33.0), (100.0, 40.0, 40.0), (100.0, 40.0, 400.0)]
        .iter()
        .map(|&(x, y, t)| StrokePoint { time: t, ..pt(x, y, 1.0) })
        .collect();
    let held: Vec<StrokePoint> = (0..8).map(|i| StrokePoint { time: i as f64 * 5.0, ..pt(if i == 0 { 0.0 } else { 60.0 }, 10.0, 1.0) }).collect();
    let backwards: Vec<StrokePoint> =
        [(0.0, 0.0, 50.0), (30.0, 0.0, 10.0), (60.0, 5.0, f64::NAN), (90.0, 5.0, 2000.0)].iter().map(|&(x, y, t)| StrokePoint { time: t, ..pt(x, y, 1.0) }).collect();
    for cfg in [
        Smoothing { amount: 0.8, ..Default::default() },
        Smoothing { amount: 0.8, catch_up: false, catch_up_on_end: false, ..Default::default() },
        Smoothing { amount: 0.3, catch_up_on_end: false, ..Default::default() },
        Smoothing { amount: 0.5, pulled_string: true, ..Default::default() },
        Smoothing { amount: 0.0, ..Default::default() },
        Smoothing { amount: 1.0, ..Default::default() },
    ] {
        for p in [&timed, &held, &backwards, &sets[1], &sets[3], &sets[4]] {
            smoothx_line(&cfg, 1.0, p);
        }
    }
    // ---- tail dabs, wants_time, smoothing string ----
    let tb = [
        b(),
        BrushSettings { smoothing: Smoothing { amount: 0.6, ..Default::default() }, ..b() },
        BrushSettings { smoothing: Smoothing { amount: 0.4, pulled_string: true, ..Default::default() }, ..b() },
        BrushSettings { build_up: true, build_up_rate: 40.0, ..b() },
        BrushSettings { aliased: true, size: 1.0, smoothing: Smoothing { amount: 0.5, ..Default::default() }, ..b() },
    ];
    for bb in &tb {
        for p in [&timed, &held, &sets[1]] {
            tdabs_line(bb, p);
        }
    }
    // ---- new brushes through the existing line kinds ----
    let rb = r7_brushes();
    let lines: Vec<Vec<StrokePoint>> = vec![
        vec![pt(2.5, 2.5, 1.0), pt(22.5, 9.5, 1.0)],
        vec![pt(3.5, 30.5, 1.0), pt(10.5, 5.5, 1.0)],
        vec![pt(1.2, 1.7, 1.0), pt(25.8, 4.1, 1.0), pt(25.8, 4.1, 1.0), pt(20.3, 30.6, 0.5)],
        vec![pt(-4.5, 7.5, 1.0), pt(600.0, 7.5, 1.0)],
        vec![pt(f64::NAN, 3.0, 1.0), pt(5.0, 3.0, 1.0), pt(9.0, 8.0, 1.0)],
    ];
    for bb in &rb {
        for p in sets.iter().chain(lines.iter()) {
            dabs_line(bb, &[], p);
        }
    }
    for (i, bb) in rb.iter().enumerate() {
        for (k, scen) in [0usize, 2, 7, 9, 5, 12].into_iter().enumerate() {
            let p = if k % 2 == 0 { &sets[(i + k) % sets.len()] } else { &lines[(i + k) % 3] };
            stroke_line(bb, &[], scen, p);
        }
    }
    for bb in &rb {
        for (c, r) in [((5.5, 5.5), 0.5f32), ((5.75, 5.25), 1.25), ((10.0, 10.0), 2.9), ((-3.3, 7.7), 3.05), ((20.5, 20.5), 9.0)] {
            let mut d = Dab::round(Point::new(c.0, c.1), r, 0.8);
            d.roundness = 0.6;
            d.angle = 0.4;
            d.depth = 0.7;
            rast_line(bb, &[], &d, false);
            d.angle = 0.0;
            d.roundness = 1.0;
            rast_line(bb, &[], &d, false);
        }
    }
    // ---- live union of symmetry passes ----
    let ups: Vec<StrokePoint> =
        (0..24).map(|i| StrokePoint { time: i as f64 * 12.0, ..pt(30.0 + i as f64 * 5.5, 40.0 + 25.0 * (i as f64 / 4.0).sin(), 0.5 + (i % 5) as f32 * 0.1) }).collect();
    let ubs = [
        BrushSettings { size: 14.0, hardness: 0.6, opacity: 0.6, ..b() },
        BrushSettings { size: 14.0, smoothing: Smoothing { amount: 0.7, ..Default::default() }, ..b() },
        BrushSettings { size: 12.0, wet_edges: true, ..b() },
        BrushSettings { size: 10.0, mode: BlendMode::Dissolve, ..b() },
        BrushSettings { size: 16.0, erase: true, ..b() },
        BrushSettings { size: 18.0, color_dynamics: ColorDynamics { enabled: true, hue_jitter: 0.5, ..Default::default() }, ..b() },
        BrushSettings { size: 18.0, dual_brush: DualBrush { enabled: true, size: 6.0, scatter: 1.5, ..Default::default() }, ..b() },
        BrushSettings { size: 18.0, mode: BlendMode::Multiply, texture: Texture { enabled: true, ..Default::default() }, ..b() },
    ];
    for (i, ub) in ubs.iter().enumerate() {
        for (scen, npass, chunk) in [(0usize, 2usize, 1usize), (2, 4, 3), (7, 4, 5), (12, 3, 2), ((i + 3) % SCENARIOS, 4, 24)] {
            ulive_line(ub, scen, npass, chunk, &ups);
        }
    }
    // ---- serde ----
    for bb in &rb {
        l!("ser", "BrushSettings", bj(bb));
    }
    let loaded = BrushSettings { tip: TipShape::Stored(r7_stored(12, true)), ..b() };
    l!("ser", "BrushSettings", bj(&loaded));
    l!(
        "ser",
        "BrushPreset",
        serde_json::to_string(&BrushPreset { name: "f".into(), brush: b(), builtin: false, group: "G".into(), folder: vec!["a".into(), "b c".into()] }).unwrap()
    );
    for (ty, s) in r7_json_cases() {
        r7_de_line(ty, &s);
    }
}
