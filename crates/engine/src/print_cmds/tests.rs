use super::*;
use photocraft_doc::{Knot, NamedPath, Path, Subpath};

fn tmp(name: &str) -> String {
    let d = std::env::temp_dir().join(format!("photocraft-print-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d.to_string_lossy().into_owned()
}

fn session(mode: &str, depth: u32) -> Session {
    let mut s = Session::new();
    s.execute("file.new", json!({"width": 300, "height": 150, "mode": mode, "depth": depth, "name": "Print Me.psd", "background": "#3366cc"})).unwrap();
    s
}

fn find(h: &[u8], n: &[u8], from: usize) -> usize {
    from + h[from..].windows(n.len()).position(|w| w == n).unwrap()
}

/// The (inflated) image stream of a print PDF: (width, height, colour space, samples).
fn pdf_image(pdf: &[u8]) -> (u32, u32, String, Vec<u8>) {
    let i = find(pdf, b"/Subtype /Image", 0);
    let end = find(pdf, b">>\nstream\n", i);
    let dict = String::from_utf8_lossy(&pdf[i..end]).into_owned();
    let num = |k: &str| dict.split(k).nth(1).unwrap().split_whitespace().next().unwrap().parse::<u32>().unwrap();
    let cs = dict.split("/ColorSpace ").nth(1).unwrap().split(" /BitsPerComponent").next().unwrap().to_string();
    let len = num("/Length ") as usize;
    let start = end + 10;
    let mut out = Vec::new();
    std::io::Read::read_to_end(&mut flate2::read::ZlibDecoder::new(&pdf[start..start + len]), &mut out).unwrap();
    (num("/Width "), num("/Height "), cs, out)
}

#[test]
fn print_dry_run_renders_a_pdf_and_reports_the_platform_backend() {
    let dir = tmp("print");
    for depth in [8, 16, 32] {
        let mut s = session("rgb", depth);
        let out = format!("{dir}/rgb-{depth}.pdf");
        let r = s
            .execute(
                "file.print",
                json!({"output": out, "send": true, "dryRun": true, "printer": "Office", "copies": 2, "paper": "a4", "cornerCropMarks": true, "registrationMarks": true, "labels": true}),
            )
            .unwrap();
        let cmd: Vec<String> = serde_json::from_value(r["command"].clone()).unwrap();
        #[cfg(unix)]
        {
            assert_eq!(&cmd[..5], &["lp", "-d", "Office", "-n", "2"]);
            assert!(cmd.contains(&"fit-to-page=false".into()));
            assert!(!cmd.contains(&out), "PDF bytes are streamed instead of passing a path");
        }
        #[cfg(windows)]
        assert!(cmd.last().unwrap().contains("craft-print PDF spooler"));
        assert_eq!(r["sent"], false);
        assert_eq!(r["pdf"], out);
        let pdf = std::fs::read(&out).unwrap();
        assert!(pdf.starts_with(b"%PDF-1.4"));
        assert!(String::from_utf8_lossy(&pdf).contains("/MediaBox [0 0 595.28 841.89]"));
        let (w, h, cs, data) = pdf_image(&pdf);
        assert_eq!((w, h), (300, 150));
        assert_eq!(cs, "/DeviceRGB");
        assert_eq!(&data[..3], &[0x33, 0x66, 0xcc]);
        // 300 px at 72 ppi = 300 pt, centred.
        let rect: Vec<f64> = serde_json::from_value(r["imageRect"].clone()).unwrap();
        assert!((rect[2] - 300.0).abs() < 1e-6 && (rect[0] - (595.28 - 300.0) / 2.0).abs() < 1e-6);
    }
    // Print to PDF: no spooling by default.
    let mut s = session("gray", 8);
    let out = format!("{dir}/out.pdf");
    let r = s.execute("file.print", json!({"output": out, "scaleToFit": true, "orientation": "landscape"})).unwrap();
    assert_eq!(r["sent"], false);
    assert!(r["command"].is_null());
    let (_, _, cs, _) = pdf_image(&std::fs::read(&out).unwrap());
    assert_eq!(cs, "/DeviceGray");
    let scale = r["scale"].as_f64().unwrap();
    assert!(scale > 200.0, "fit to a landscape letter page: {scale}");
    assert!(s.execute("file.print", json!({"paper": "napkin", "dryRun": true})).is_err());
    assert!(s.execute("file.print", json!({"colorHandling": "photocraftManages", "dryRun": true})).is_err(), "needs a printer profile");
}

#[test]
fn photocraft_manages_colors_converts_to_the_printer_profile() {
    let mut s = session("rgb", 8);
    let out = format!("{}/managed.pdf", tmp("managed"));
    let r = s
        .execute(
            "file.print",
            json!({"output": out, "dryRun": true, "colorHandling": "photocraftManages", "printerProfile": "coated-cmyk", "intent": "perceptual", "bpc": false}),
        )
        .unwrap();
    assert_eq!(r["color"]["intent"], "perceptual");
    let pdf = std::fs::read(&out).unwrap();
    let (_, _, cs, data) = pdf_image(&pdf);
    assert_eq!(cs, "[/ICCBased 7 0 R]");
    assert!(String::from_utf8_lossy(&pdf).contains("/N 4 /Alternate /DeviceCMYK"));
    assert_eq!(data.len(), 300 * 150 * 4);
    // Blue: lots of cyan, little yellow.
    assert!(data[0] > 150 && data[2] < 60, "{:?}", &data[..4]);
    // The document itself is untouched.
    assert_eq!(s.active().unwrap().doc.mode, ColorMode::Rgb);
}

/// The inflated page content stream (object 4).
fn pdf_content(pdf: &[u8]) -> String {
    let i = find(pdf, b"4 0 obj\n", 0);
    let len_at = find(pdf, b"/Length ", i) + 8;
    let len: usize = String::from_utf8_lossy(&pdf[len_at..len_at + 12]).split_whitespace().next().unwrap().parse().unwrap();
    let start = find(pdf, b"stream\n", i) + 7;
    let mut out = String::new();
    std::io::Read::read_to_string(&mut flate2::read::ZlibDecoder::new(&pdf[start..start + len]), &mut out).unwrap();
    out
}

#[test]
fn emulsion_down_mirrors_the_whole_page() {
    let mut s = session("rgb", 8);
    let dir = tmp("emulsion");
    let plain_path = format!("{dir}/plain.pdf");
    let mirrored_path = format!("{dir}/mirrored.pdf");
    let again_path = format!("{dir}/again.pdf");
    let plain = s
        .execute("file.print", json!({"output": plain_path, "dryRun": true, "paper": "a4", "center": false, "left": 1.0, "top": 1.0, "labels": true}))
        .unwrap();
    let mirrored = s
        .execute(
            "file.print",
            json!({"output": mirrored_path, "dryRun": true, "paper": "a4", "center": false, "left": 1.0, "top": 1.0, "labels": true, "emulsionDown": true}),
        )
        .unwrap();
    assert_eq!(plain["emulsionDown"], false);
    assert_eq!(mirrored["emulsionDown"], true);
    // Same layout; only the page is flipped.
    assert_eq!(plain["imageRect"], mirrored["imageRect"]);
    let (p, m) = (std::fs::read(plain["pdf"].as_str().unwrap()).unwrap(), std::fs::read(mirrored["pdf"].as_str().unwrap()).unwrap());
    let (pc, mc) = (pdf_content(&p), pdf_content(&m));
    assert!(!pc.contains("-1 0 0 1"));
    // The flip wraps the image and the label, after the white paper, and is closed again.
    let flip = mc.find("q -1 0 0 1 595.28 0 cm").expect("mirror transform");
    assert!(flip > mc.find("re f Q").unwrap() && flip < mc.find("/Im0 Do").unwrap() && flip < mc.find("Tj").unwrap());
    assert_eq!(mc.matches("q ").count() + mc.matches("q\n").count(), mc.matches("Q\n").count(), "balanced q/Q: {mc}");
    // The pixels themselves are not touched (the transform mirrors them).
    assert_eq!(pdf_image(&p).3, pdf_image(&m).3);
    // Print One Copy keeps it.
    let again = s.execute("file.printOneCopy", json!({"output": again_path, "dryRun": true})).unwrap();
    assert_eq!(again["emulsionDown"], true);
    for r in [plain, mirrored, again] {
        let _ = std::fs::remove_file(r["pdf"].as_str().unwrap());
    }
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn printer_profile_can_be_an_installed_icc_file() {
    let dir = tmp("icc");
    let icc = format!("{dir}/My Paper.icc");
    let output = format!("{dir}/managed.pdf");
    std::fs::write(&icc, photocraft_cms::Builtin::CoatedCmyk.profile().to_bytes().as_slice()).unwrap();
    let mut s = session("rgb", 16);
    let r = s
        .execute("file.print", json!({"output": output, "dryRun": true, "colorHandling": "photocraftManages", "printerProfile": icc, "emulsionDown": true}))
        .unwrap();
    assert_eq!(r["color"]["printerProfile"], json!(icc));
    let (_, _, cs, data) = pdf_image(&std::fs::read(r["pdf"].as_str().unwrap()).unwrap());
    assert_eq!(cs, "[/ICCBased 7 0 R]");
    assert_eq!(data.len(), 300 * 150 * 4);
    assert!(s.execute("file.print", json!({"dryRun": true, "colorHandling": "photocraftManages", "printerProfile": format!("{dir}/missing.icc")})).is_err());
    let _ = std::fs::remove_file(r["pdf"].as_str().unwrap());
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn print_one_copy_repeats_the_last_settings() {
    let mut s = session("rgb", 8);
    let first = s.execute("file.print", json!({"dryRun": true, "copies": 3, "paper": "a5", "scale": 50})).unwrap();
    assert!(first["pdf"].is_null());
    let r = s.execute("file.printOneCopy", json!({"dryRun": true})).unwrap();
    assert_eq!(r["copies"], 1);
    assert_eq!(r["paper"], json!([419.53, 595.28]));
    assert_eq!(r["scale"], 50.0);
    let cmd: Vec<String> = serde_json::from_value(r["command"].clone()).unwrap();
    #[cfg(unix)]
    assert_eq!(&cmd[..3], &["lp", "-n", "1"]);
    #[cfg(windows)]
    assert!(cmd.last().unwrap().contains("craft-print PDF spooler"));
    assert!(r["pdf"].is_null());
}

#[test]
fn package_copies_links_and_relinks() {
    let dir = tmp("package");
    let link = format!("{dir}/art.png");
    let mut src = Session::new();
    src.execute("file.new", json!({"width": 20, "height": 20, "background": "#ff0000"})).unwrap();
    crate::file_cmds::save_doc(&src.active().unwrap().doc, &link, None).unwrap();
    let mut s = session("rgb", 8);
    s.execute("file.placeLinked", json!({"path": link})).unwrap();
    let r = s.execute("file.package", json!({"dir": format!("{dir}/pkg")})).unwrap();
    let copied = format!("{dir}/pkg/Print Me/Links/art.png");
    assert_eq!(r["links"], json!([copied]));
    assert!(std::path::Path::new(&copied).is_file());
    let docp = r["document"].as_str().unwrap();
    assert!(docp.ends_with("Print Me/Print Me.pcraft"));
    let back = photocraft_io::import("Print Me.pcraft", &std::fs::read(docp).unwrap()).unwrap().document;
    let linked: Vec<String> = back
        .walk()
        .iter()
        .filter_map(|(_, _, l)| match &l.content {
            LayerContent::Smart(so) => match &so.source {
                SmartSource::Linked { path } => Some(path.clone()),
                _ => None,
            },
            _ => None,
        })
        .collect();
    assert_eq!(linked.len(), 1);
    assert!(linked[0].ends_with("Links/art.png"), "{linked:?}");
    // The open document still points at the original.
    let open = &s.active().unwrap().doc;
    assert!(
        open.walk()
            .iter()
            .any(|(_, _, l)| matches!(&l.content, LayerContent::Smart(so) if matches!(&so.source, SmartSource::Linked { path } if *path == link)))
    );
}

#[test]
fn paths_export_as_illustrator_postscript() {
    let mut s = session("rgb", 8);
    assert!(!s.is_enabled("file.export.pathsToIllustrator"));
    s.edit("paths", |d, _| {
        let tri = Subpath::polygon(&[(10.0, 10.0), (100.0, 10.0), (50.0, 90.0)]);
        let mut curve = Subpath::polyline(&[(0.0, 0.0), (300.0, 150.0)]);
        curve.knots[0] = Knot::smooth(photocraft_geom::Point::new(0.0, 0.0), photocraft_geom::Point::new(0.0, 0.0), photocraft_geom::Point::new(50.0, 0.0));
        d.paths.push(NamedPath { name: "Shapes".into(), path: Path::new(vec![tri, curve]), psd_raw: None });
        Ok(())
    })
    .unwrap();
    let r = s.execute("file.export.pathsToIllustrator", json!({})).unwrap();
    let ai = r["ai"].as_str().unwrap();
    assert!(ai.starts_with("%!PS-Adobe-2.0 EPSF-1.2"));
    assert!(ai.contains("%%BoundingBox: 0 0 300 150"));
    assert!(ai.contains("*u\n10.0000 140.0000 m\n100.0000 140.0000 L\n"), "{ai}");
    assert!(ai.contains("\nn\n") && ai.contains("\nN\n*U\n"));
    assert!(ai.contains(" C\n"), "curve segment");
    let dir = tmp("ai");
    let r = s.execute("file.export.pathsToIllustrator", json!({"path": format!("{dir}/p.ai"), "paths": "Shapes"})).unwrap();
    assert_eq!(r["paths"], 1);
    assert!(s.execute("file.export.pathsToIllustrator", json!({"paths": "Nope"})).is_err());
}

#[test]
fn print_dry_run_without_output_retains_no_pdf() {
    let mut s = session("rgb", 8);
    let result = s.execute("file.print", json!({"dryRun": true})).unwrap();
    assert!(result["pdf"].is_null());
    assert_eq!(result["sent"], false);
    assert!(result["bytes"].as_u64().unwrap() > 100);
    assert!(result["command"].is_array());
    assert!(s.file_menu.last_print.as_ref().unwrap().get("dryRun").is_none());
}

#[test]
fn print_without_sending_requires_output_unless_dry_run_and_forgets_send_override() {
    let mut s = session("rgb", 8);
    let before = s.file_menu.last_print.clone();
    let err = s.execute("file.print", json!({"send": false})).unwrap_err();
    assert!(err.to_string().contains("needs an output path"), "{err}");
    assert_eq!(s.file_menu.last_print, before);
    let result = s.execute("file.print", json!({"send": false, "dryRun": true})).unwrap();
    assert!(result["pdf"].is_null());
    assert!(result["command"].is_null());
    assert!(s.file_menu.last_print.as_ref().unwrap().get("send").is_none());
    let out = format!("{}/export.pdf", tmp("non-sending-export"));
    let result = s.execute("file.print", json!({"output": out, "send": false})).unwrap();
    assert_eq!(result["sent"], false);
    assert!(std::fs::read(&out).unwrap().starts_with(b"%PDF-1.4"));
    assert!(s.file_menu.last_print.as_ref().unwrap().get("send").is_none());
    s.execute("file.print", json!({"output": out, "send": true, "dryRun": true})).unwrap();
    assert!(s.file_menu.last_print.as_ref().unwrap().get("send").is_none());
}
/// parallel tests are untouched, and the real spool boundary still consumes the PDF bytes.
#[cfg(unix)]
#[test]
fn spool_transport_streams_pdf_and_propagates_refusal() {
    const CHILD: &str = "PHOTOCRAFT_TEST_PRINT_TRANSPORT";
    if let Ok(dir) = std::env::var(CHILD) {
        let mut s = session("rgb", 8);
        let source = s.active().unwrap().doc.id;
        s.execute("file.scripts.scriptEventsManager", json!({"add": {"event": "print", "steps": [["image.imageRotation.90cw", {}]]}})).unwrap();
        let crate::jobs::Started::Job(job) = s.start("file.print", json!({"printer": "Office", "copies": 3, "paper": "a4", "scale": 50})).unwrap() else {
            panic!("desktop printing must start on a worker");
        };
        assert!(!s.cancel_job(job), "queue side effects must not be reported as cancelled");
        assert_eq!(s.execute("jobs.cancel", json!({})).unwrap()["cancelled"], 0);
        assert!(s.execute("jobs.cancel", json!({"job": job.0})).unwrap_err().to_string().contains("system print queue"));
        s.execute("file.new", json!({"width": 40, "height": 10})).unwrap();
        let viewed = s.active().unwrap().doc.id;
        let journal_before_completion = s.journal.len();
        let out = s.wait_job(job).unwrap();
        assert_eq!(s.journal.len(), journal_before_completion + 1, "print hooks must not become duplicate recorded action steps");
        assert_eq!(s.journal.last().unwrap().0, "file.print");
        assert_eq!(s.active().unwrap().doc.id, viewed, "completion keeps the user's tab");
        assert_eq!(s.active().unwrap().doc.size.width, 40, "print hook must not act on the newly viewed document");
        let printed = &s.documents().iter().find(|st| st.doc.id == source).unwrap().doc;
        assert_eq!((printed.size.width, printed.size.height), (150, 300), "print hook ran on the source document");
        assert_eq!(out["sent"], true);
        assert_eq!(out["spooler"], "request id is Office-42");
        assert!(out["pdf"].is_null(), "spool-only printing leaves no diagnostic PDF");
        let pdf = std::fs::read(format!("{dir}/received.pdf")).unwrap();
        assert!(pdf.starts_with(b"%PDF-1.4"));
        assert!(String::from_utf8_lossy(&pdf).contains("/MediaBox [0 0 595.28 841.89]"));
        let args = std::fs::read_to_string(format!("{dir}/args")).unwrap();
        assert!(args.contains("fit-to-page=false"));
        assert!(args.contains("-n\n3\n"));
        assert!(!args.contains(".pdf"));
        let remembered = s.file_menu.last_print.clone();
        let explicit = format!("{dir}/saved.pdf");
        let err = s.execute("file.print", json!({"printer": "Refuse", "output": explicit, "send": true})).unwrap_err();
        assert!(err.to_string().contains("queue refused"), "{err}");
        assert!(std::fs::read(&explicit).unwrap().starts_with(b"%PDF-1.4"), "explicit export survives a spool refusal");
        assert_eq!(s.file_menu.last_print, remembered, "failed printing does not replace repeat settings");
        // Export-only choices must not disable submission when Print One Copy repeats settings.
        let export = format!("{dir}/export-only.pdf");
        s.execute("file.print", json!({"printer": "Office", "output": export, "send": false})).unwrap();
        let remembered = s.file_menu.last_print.as_ref().unwrap();
        assert!(remembered.get("send").is_none() && remembered.get("output").is_none());
        let repeat = s.execute("file.printOneCopy", json!({})).unwrap();
        assert_eq!(repeat["sent"], true);
        assert_eq!(repeat["copies"], 1);
        assert!(repeat["pdf"].is_null());
        assert!(!std::fs::read_to_string(format!("{dir}/args")).unwrap().contains(".pdf"));
        return;
    }
    use std::os::unix::fs::PermissionsExt;
    let dir = tempfile::tempdir().unwrap();
    let script = dir.path().join("lp");
    std::fs::write(
        &script,
        b"#!/bin/sh\nif [ \"$2\" = Refuse ]; then echo 'queue refused' >&2; exit 1; fi\nprintf '%s\\n' \"$@\" > \"$PHOTOCRAFT_TEST_PRINT_TRANSPORT/args\"\n/bin/cat > \"$PHOTOCRAFT_TEST_PRINT_TRANSPORT/received.pdf\"\necho 'request id is Office-42 (0 file(s))'\n",
    ).unwrap();
    std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o700)).unwrap();
    let result = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "print_cmds::tests::spool_transport_streams_pdf_and_propagates_refusal", "--nocapture"])
        .env(CHILD, dir.path())
        .env("PATH", dir.path())
        // The child spools into the fake `lp` above, so a shell that blocks real printing
        // (`PHOTOCRAFT_NO_PRINT`, see `disabled_printing_never_spools`) must not stop it.
        .env_remove("PHOTOCRAFT_NO_PRINT")
        .output()
        .unwrap();
    assert!(result.status.success(), "{}\n{}", String::from_utf8_lossy(&result.stdout), String::from_utf8_lossy(&result.stderr));
    assert!(String::from_utf8_lossy(&result.stdout).contains("1 passed"), "child must select the transport test");
}

#[test]
fn disabled_printing_never_spools() {
    let mut s = Session::new();
    s.execute("file.new", json!({"width": 8, "height": 8})).unwrap();
    super::disable_printing();
    assert!(super::printing_disabled());
    let e = s.execute("file.print", json!({})).unwrap_err().to_string();
    assert!(e.contains("printing is disabled"), "{e}");
    assert!(s.execute("file.printOneCopy", json!({})).is_err());
    // A dry run still renders and reports the command it would have run.
    let r = s.execute("file.print", json!({"dryRun": true})).unwrap();
    assert_eq!(r["sent"], false);
    assert!(r["command"].is_array(), "{r}");
}

#[test]
fn printers_lists_the_spooler_destinations_without_a_document() {
    let mut s = Session::new();
    let r = s.execute("file.printers", json!({})).unwrap();
    let list = r["printers"].as_array().unwrap();
    assert!(list.iter().all(|p| p["name"].is_string() && p["default"].is_boolean()));
    match r["default"].as_str() {
        Some(d) => assert!(list.iter().any(|p| p["name"] == d && p["default"] == true)),
        None => assert!(list.iter().all(|p| p["default"] == false)),
    }
}
