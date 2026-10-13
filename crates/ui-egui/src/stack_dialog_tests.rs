use egui::vec2;
use egui_kittest::Harness;
use egui_kittest::kittest::{NodeT, Queryable};

use super::*;
use crate::file_dialog::{FileDialogAnswer, FileDialogRequest};

/// `n` small PNGs named `names` in a fresh folder for `tag`: (folder, paths).
fn pngs(tag: &str, names: &[&str]) -> (String, Vec<String>) {
    let dir = std::env::temp_dir().join(format!("photocraft-load-stack-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let mut s = photocraft_engine::Session::new();
    s.execute("file.new", json!({"width": 8, "height": 6, "background": "#406080"})).unwrap();
    let paths = names
        .iter()
        .map(|n| {
            let p = dir.join(n).to_string_lossy().into_owned();
            s.execute("file.saveACopy", json!({"path": p})).unwrap();
            p
        })
        .collect();
    (dir.to_string_lossy().into_owned(), paths)
}

fn harness(answers: Vec<Option<FileDialogAnswer>>) -> (Harness<'static, PhotocraftApp>, std::rc::Rc<std::cell::RefCell<Vec<FileDialogRequest>>>) {
    let (show, asked) = crate::file_dialog::fake(answers);
    let h = Harness::builder().with_size(vec2(1440.0, 900.0)).with_max_steps(64).build_eframe(move |cc| {
        PhotocraftApp::setup_context(&cc.egui_ctx, Default::default());
        PhotocraftApp::new(photocraft_engine::Session::new(), crate::Services { file_dialog: Some(show), ..Default::default() })
    });
    (h, asked)
}

fn dialog(h: &Harness<'_, PhotocraftApp>) -> Option<(u64, Map<String, Value>)> {
    h.state().ui.dialogs.iter().find(|d| owns(&d.fields)).map(|d| (d.id, d.fields.clone()))
}

fn listed(h: &Harness<'_, PhotocraftApp>) -> Vec<String> {
    dialog(h).map(|(_, f)| files(&f).iter().map(|p| file_name(p).to_string()).collect()).unwrap_or_default()
}

/// #4671 / #4696: the menu opens Photoshop's dialog, Browse… adds files (once each), Remove,
/// Sort By Name and Add Open Files edit the list, and OK loads it.
#[test]
fn source_files_list_is_built_with_browse_remove_sort_and_open_files() {
    let (_, paths) = pngs("files", &["b.png", "c.png", "a.png"]);
    let (mut h, asked) = harness(vec![
        Some(FileDialogAnswer::Paths(vec![paths[0].clone(), paths[1].clone()])),
        Some(FileDialogAnswer::Paths(vec![paths[2].clone(), paths[0].clone()])),
    ]);
    let ctx = h.ctx.clone();
    crate::menus::invoke(h.state_mut(), &ctx, COMMAND, json!({})).unwrap();
    h.run_steps(4);
    assert!(dialog(&h).is_some(), "the menu opens the dialog");
    for label in [
        "Source Files",
        "Browse…",
        "Remove",
        "Add Open Files",
        "Sort By Name",
        "Attempt to Automatically Align Source Images",
        "Create Smart Object after Loading Layers",
    ] {
        assert!(h.query_by_label(label).is_some(), "{label}");
    }
    assert!(h.get_by_label("OK").accesskit_node().is_disabled(), "nothing to load yet");
    assert!(h.get_by_label("Remove").accesskit_node().is_disabled());

    h.get_by_label("Browse…").click();
    h.run_steps(4);
    assert!(matches!(asked.borrow()[0], FileDialogRequest::Open { multiple: true, .. }), "{:?}", asked.borrow());
    assert_eq!(listed(&h), ["b.png", "c.png"]);
    h.get_by_label("Browse…").click();
    h.run_steps(4);
    assert_eq!(listed(&h), ["b.png", "c.png", "a.png"], "b.png only once");

    h.get_by_label("Sort By Name").click();
    h.run_steps(2);
    assert_eq!(listed(&h), ["a.png", "b.png", "c.png"]);
    h.get_by_label("b.png").click();
    h.run_steps(2);
    h.get_by_label("Remove").click();
    h.run_steps(2);
    assert_eq!(listed(&h), ["a.png", "c.png"]);

    // Add Open Files: the open documents that have a file (b.png, already saved; not the untitled one).
    h.state_mut().run("file.new", json!({"width": 4, "height": 4})).unwrap();
    h.state_mut().session.active_mut().unwrap().path = Some(paths[0].clone());
    h.state_mut().run("file.new", json!({"width": 4, "height": 4})).unwrap();
    h.run_steps(3);
    h.get_by_label("Add Open Files").click();
    h.run_steps(2);
    assert_eq!(listed(&h), ["a.png", "c.png", "b.png"]);

    let docs = h.state().session.documents().len();
    h.get_by_label("OK").click();
    h.run_steps(4);
    assert!(dialog(&h).is_none());
    assert_eq!(h.state().session.documents().len(), docs + 1, "a new document");
    let d = &h.state().session.active().unwrap().doc;
    let names: Vec<&str> = d.layers.iter().map(|l| l.name.as_str()).collect();
    assert_eq!(names, ["a.png", "c.png", "b.png"], "one layer per file, in the list's order");
}

/// Use: Folder loads the images of the folder Browse… chose, listed in the dialog; with Create
/// Smart Object they go into one smart object.
#[test]
fn a_folder_loads_its_images() {
    let (dir, _) = pngs("folder", &["one.png", "two.png"]);
    let (mut h, asked) = harness(vec![Some(FileDialogAnswer::FolderTo(dir.clone()))]);
    let id = open(h.state_mut());
    h.run_steps(3);
    h.state_mut().ui.dialog_mut(id).unwrap().fields.insert("use".into(), json!("folder"));
    h.run_steps(2);
    assert!(h.get_by_label("Remove").accesskit_node().is_disabled() && h.get_by_label("Add Open Files").accesskit_node().is_disabled());
    h.get_by_label("Browse…").click();
    h.run_steps(4);
    assert!(matches!(asked.borrow()[0], FileDialogRequest::Folder { .. }));
    assert!(h.query_by_label("one.png").is_some() && h.query_by_label("two.png").is_some(), "the folder's images are listed");
    // Create Smart Object after Loading Layers: both images inside one smart object.
    h.get_by_label("Create Smart Object after Loading Layers").click();
    h.run_steps(2);
    crate::dialogs::confirm(h.state_mut(), id).unwrap();
    let d = &h.state().session.active().unwrap().doc;
    assert_eq!(d.layers.len(), 1);
    assert!(matches!(d.layers[0].content, photocraft_doc::LayerContent::Smart(_)));
}
