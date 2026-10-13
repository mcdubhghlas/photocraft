//! `rustref text-vectors`, `# r8` section: what the text crate gained after L13/L14 (resync R8):
//! face style / PostScript names, named faces, charmap_for_style, localized family names, the
//! Southeast Asian fallback lists, missing glyph protection, scripts.rs. Replayed by
//! tests/text/test_r8_parity.c. Lines start with `r8`; older C tests ignore the section.

use photocraft_doc::text::CharStyle;
use photocraft_doc::TextLayer;
use photocraft_text::{fonts, scripts, FontDb, TextEngine};

use crate::text_vectors::{esc, fnv, font_files, hash_layout, hu32, x32, H0};

fn opt(s: &Option<String>) -> String {
    s.as_deref().map_or_else(|| "-".to_string(), esc)
}

fn print_names(tag: &str, db: &mut FontDb, fam: &str) {
    let mut s = String::new();
    for f in db.faces(fam) {
        s.push_str(&format!(" {}|{}|{}|{}", esc(&f.style), opt(&f.postscript_name), x32(f.weight), u8::from(f.italic)));
    }
    println!("r8names {tag} {} =>{s}", esc(fam));
}

fn named() {
    let mut db = FontDb::new();
    for f in db.families() {
        print_names("bundled", &mut db, &f);
    }
    for (fam, style) in [("Inter", "Medium"), ("inter", "semibold"), ("Inter", "Bold"), ("JetBrains Mono", "Regular"), ("Inter", ""), ("missing", "20")] {
        match db.named_face(fam, style) {
            Some(f) => println!("r8named {} {} => {} {} {} {}", esc(fam), esc(style), esc(&f.family), x32(f.weight), u8::from(f.italic), opt(&f.postscript_name)),
            None => println!("r8named {} {} => none", esc(fam), esc(style)),
        }
    }
    for (spec, bytes) in font_files() {
        let mut d = FontDb::new();
        let names = d.register_font_data(bytes);
        for n in names {
            print_names(&spec, &mut d, &n);
        }
    }
}

fn charmap_styles() {
    let styles = [
        CharStyle { font_family: "Inter".into(), font_style: "SemiBold".into(), weight: 600, ..Default::default() },
        CharStyle { font_family: "Inter".into(), postscript_name: Some("Inter-Medium".into()), ..Default::default() },
        CharStyle { font_family: "Inter".into(), font_style: "Regular".into(), weight: 700, ..Default::default() },
        CharStyle { font_family: "JetBrains Mono".into(), ..Default::default() },
        CharStyle { font_family: "No Such".into(), ..Default::default() },
    ];
    let mut db = FontDb::new();
    for (i, s) in styles.iter().enumerate() {
        let cm = db.charmap_for_style(s);
        let mut h = H0;
        for c in &cm {
            hu32(&mut h, *c as u32);
        }
        println!("r8cmstyle {i} => {} {h:016x}", cm.len());
    }
}

fn localized() {
    let cases: Vec<(&str, Vec<(&str, Option<&str>)>)> = vec![
        ("MS Gothic", vec![("MS Gothic", Some("en-US")), ("ＭＳ ゴシック", Some("ja-JP"))]),
        ("Microsoft YaHei", vec![("Microsoft YaHei", Some("en")), ("微软雅黑", Some("zh-CN")), ("微軟雅黑", Some("zh-TW"))]),
        ("Arial", vec![("Arial", Some("en-US"))]),
        ("Arial", vec![("Arial", Some("en-GB")), ("Arial", Some("de-DE"))]),
        ("X", vec![("Y", None), ("  ", Some("ja-JP"))]),
        ("X", vec![]),
        ("X", vec![("Y", Some("EN_gb")), ("Z", Some("")), ("W", Some("fr")), ("V", Some("ja"))]),
        ("X", vec![("\u{3000}\t", Some("ja")), ("X", Some("ja")), ("Q", Some("ko-KR"))]),
    ];
    for (i, (english, names)) in cases.iter().enumerate() {
        let names: Vec<(String, Option<String>)> = names.iter().map(|(n, l)| (n.to_string(), l.map(str::to_string))).collect();
        println!("r8locname {i} => {}", fonts::localized_name(english, &names).map_or_else(|| "-".to_string(), |s| esc(&s)));
    }
    let mut db = FontDb::new();
    for (_, bytes) in font_files() {
        db.register_font_data(bytes);
    }
    let mut v: Vec<(String, String)> = db.localized_family_names().into_iter().collect();
    v.sort();
    println!("r8localized => {}", v.iter().map(|(a, b)| format!("{}={}", esc(a), esc(b))).collect::<Vec<_>>().join(" "));
}

fn script_name(s: scripts::Script) -> String {
    format!("{s:?}")
}

fn scripts_section() {
    let mut h = H0;
    for c in 0u32..0x20000 {
        let k = char::from_u32(c).and_then(scripts::classify).map_or(255u8, |s| scripts::Script::ALL.iter().position(|x| *x == s).unwrap_or(254) as u8);
        fnv(&mut h, &[k]);
    }
    println!("r8sclassify => {h:016x}");
    let files: Vec<std::path::PathBuf> = [
        "/usr/share/fonts/google-noto/NotoSansThai-Bold.ttf",
        "/usr/share/fonts/google-noto-vf/NotoSansThai[wght].ttf",
        "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansThaiLooped-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansThaana-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/lohit-devanagari/Lohit-Devanagari.ttf",
        "/usr/share/fonts/Lohit-Tamil.ttf",
        "/mnt/c/Windows/Fonts/LeelawUI.ttf",
        "/mnt/c/Windows/Fonts/Nirmala.ttc",
        "/System/Library/Fonts/Supplemental/Thonburi.ttc",
        "/System/Library/Fonts/Supplemental/Kohinoor.ttc",
        "/usr/share/fonts/README.txt",
        "/a/b/Noto_Sans Arabic-Italic.OTF",
        "/a/b/notosansarabic-Condensed.ttf",
        "/a/notosansarabic.ttf",
        "/a-b/NotoSansArabic.ttf",
        "/a/b/NotoSansArabicUI[wdth,wght].ttf",
        "/x/Tahoma.TTF",
        "/x/.ttf",
        "/x/arial-Black.ttf",
        "/x/arial.ttf.bak",
        "/x/SegoeUI-Light.ttf",
        "/x/segoeui.ttf",
        "/x/Kalinga-Bold.ttf",
        "/x/Lohit-Odia.ttf",
        "/x/lohit-odia.ttf",
        "/x/Padauk.ttf",
        "/x/Padauk-Regular.ttf",
        "/x/padauk[wght].ttf",
        "rel/Amiri-Regular.ttf",
        "rel//./Amiri-Regular.ttf",
        "rel/Amiri-Regular.ttf",
    ]
    .into_iter()
    .map(std::path::PathBuf::from)
    .collect();
    for s in scripts::Script::ALL {
        let c = scripts::candidates(s, &files);
        println!("r8scand {} => {}", script_name(s), c.iter().map(|p| esc(&p.display().to_string())).collect::<Vec<_>>().join(" "));
    }
    let mut checks: Vec<(String, Vec<u8>)> = font_files();
    for (i, bad) in [&b""[..], b"ttcf", b"ttcf\0\x01\0\0\0\0\0\x02\xff\xff\xff\xff\0\0\0\x10", b"\0\x01\0\0\0\xff"].iter().enumerate() {
        checks.push((format!("bad{i}"), bad.to_vec()));
    }
    for (spec, bytes) in &checks {
        let mut out = String::new();
        for c in ['A', 'ส', 'ر', 'ש', 'न', '日', 'é', '\u{0}', '😀'] {
            out.push_str(&format!(" {}", scripts::covering_face(bytes, c).map_or_else(|| "-".to_string(), |i| i.to_string())));
        }
        println!("r8covering {} =>{out}", esc(spec));
    }
}

fn layout_line(tag: &str, l: &photocraft_text::TextLayout) {
    let h = hash_layout(l);
    println!(
        "r8lay {tag} => {} {} {} {:016x} {:016x} {:016x} {:016x} {:016x} {:016x}",
        l.faces.len(),
        l.glyphs.len(),
        l.lines.len(),
        h[0],
        h[1],
        h[2],
        h[3],
        h[4],
        h[5]
    );
}

/// Missing glyph protection: off, only the default family shapes (VL Gothic and DejaVu Sans,
/// registered, would cover the Japanese and Hebrew).
fn protection() {
    let mut e = TextEngine::new();
    for p in ["/usr/share/fonts/vlgothic/VL-Gothic-Regular.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"] {
        if let Ok(b) = std::fs::read(p) {
            e.fonts.register_font_data(b);
        }
    }
    let t = TextLayer { text: "Latin 日本語 שלום".into(), font_family: "Inter".into(), size_pt: 20.0, ..Default::default() };
    println!("r8mgp {}", u8::from(e.fonts.missing_glyph_protection()));
    let on = e.layout(&t, 72.0);
    layout_line("mgp_on", &on);
    e.fonts.set_missing_glyph_protection(false);
    let off = e.layout(&t, 72.0);
    layout_line("mgp_off", &off);
    e.fonts.set_missing_glyph_protection(true);
    layout_line("mgp_again", &e.layout(&t, 72.0));
}

fn fallback_lists() {
    for (name, list) in [("thai", fonts::THAI_FAMILIES), ("khmer", fonts::KHMER_FAMILIES), ("lao", fonts::LAO_FAMILIES), ("myanmar", fonts::MYANMAR_FAMILIES)] {
        println!("r8families {name} => {}", list.iter().map(|f| esc(f)).collect::<Vec<_>>().join(" "));
    }
}

pub fn run() {
    println!("# r8");
    fallback_lists();
    named();
    charmap_styles();
    localized();
    scripts_section();
    protection();
}
