//! Layers-panel pieces for smart objects: the thumbnail badge and the Smart Filters sub-rows
//! (eye toggles; double-click a filter to re-open its dialog with the recorded parameters; click
//! to select one; drag onto another smart object to move it there, ⌥-drag to copy it, #4271).

use egui::{Align2, Color32, Rect, Sense, Stroke, StrokeKind, pos2, vec2};
use photocraft_doc::{Document, Layer, LayerContent, LayerId, SmartObject};
use serde_json::{Value, json};

use crate::theme::Tokens;
use crate::{PhotocraftApp, icons};

/// What the badge on a layer thumbnail marks the layer as.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ThumbBadge {
    Smart,
    Shape,
}

/// The badge `l`'s thumbnail carries. Smart objects and shape layers (#4584) show their pixels
/// like any other layer, so only the badge tells them apart.
fn thumb_badge_kind(l: &Layer) -> Option<ThumbBadge> {
    match l.content {
        LayerContent::Smart(_) => Some(ThumbBadge::Smart),
        LayerContent::Shape(_) => Some(ThumbBadge::Shape),
        _ => None,
    }
}

/// Where the badge sits: over the bottom-right corner of the thumbnail.
fn thumb_badge_rect(thumb: Rect) -> Rect {
    Rect::from_min_size(pos2(thumb.right() - 9.0, thumb.bottom() - 9.0), vec2(11.0, 11.0))
}

/// The shape badge's path, inset from the badge so its corner anchors stay inside the frame.
const SHAPE_PATH_INSET: f32 = 3.0;
/// Side of an anchor point on the shape badge's path.
const SHAPE_ANCHOR: f32 = 3.0;

/// Smart-object or shape-layer badge in the bottom-right corner of a layer thumbnail.
pub fn thumb_badge(ui: &egui::Ui, l: &Layer, thumb: Rect) {
    let Some(kind) = thumb_badge_kind(l) else { return };
    let t = Tokens::get(ui.ctx());
    let r = thumb_badge_rect(thumb);
    ui.painter().rect_filled(r, 2.0, t.field);
    ui.painter().rect_stroke(r, 2.0, Stroke::new(1.0, t.field_border), egui::StrokeKind::Inside);
    match kind {
        ThumbBadge::Smart => icons::paint(ui, r, "app-window", 9.0, t.icon),
        // Photoshop's shape mark: a path with an anchor point on each corner. Painted rather
        // than an icon, so the anchors stay solid squares at this size.
        ThumbBadge::Shape => {
            let path = r.shrink(SHAPE_PATH_INSET);
            ui.painter().rect_stroke(path, 0.0, Stroke::new(1.0, t.icon), egui::StrokeKind::Middle);
            for anchor in [path.left_top(), path.right_top(), path.right_bottom(), path.left_bottom()] {
                ui.painter().rect_filled(Rect::from_center_size(anchor, vec2(SHAPE_ANCHOR, SHAPE_ANCHOR)), 0.0, t.icon);
            }
        }
    }
}

/// The display name of a smart filter (its command's label without the ellipsis). A Photoshop
/// filter PhotoCraft doesn't implement shows its own name and is marked as kept as is.
fn filter_label(f: &photocraft_doc::SmartFilter) -> String {
    let command = f.command.as_str();
    if command == photocraft_engine::smart_cmds::UNSUPPORTED_FILTER {
        let name = f.params.get("name").and_then(Value::as_str).unwrap_or("Smart filter");
        return format!("{} (kept, not editable)", name.trim_end_matches("...").trim_end_matches('…'));
    }
    photocraft_engine::commands::find(command)
        .map_or_else(|| command.rsplit('.').next().unwrap_or(command).to_string(), |c| tl!(c.label).trim_end_matches('…').to_string())
}

/// Smart Filters header + one row per filter (top filter first, as in Photoshop) under a smart
/// layer. Edits are queued in `actions` as engine commands.
pub fn filter_rows(app: &mut PhotocraftApp, ui: &mut egui::Ui, l: &Layer, depth: usize, actions: &mut Vec<(String, Value)>) {
    let LayerContent::Smart(sm) = &l.content else { return };
    if sm.smart_filters.is_empty() {
        return;
    }
    let t = Tokens::get(ui.ctx());
    let indent = 30.0 + depth as f32 * 14.0 + 34.0;
    let Some(st) = app.session.active() else { return };
    let (active, doc) = (st.active_layer == Some(l.id), st.doc.id);
    let picked = ui.ctx().data(|d| d.get_temp::<FilterDrag>(selected_key(doc)));
    let rows = std::iter::once(None).chain((0..sm.smart_filters.len()).rev().map(Some));
    for index in rows {
        let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width(), 22.0), Sense::click_and_drag());
        if resp.drag_started() {
            ui.ctx().data_mut(|d| d.insert_temp::<FilterDrag>(drag_key(), (l.id.0, index)));
        }
        // Selected as in Photoshop: the row stays highlighted while its smart object is active.
        let selected = active && picked == Some((l.id.0, index));
        if selected {
            ui.painter().rect_filled(rect, 0.0, if t.pro { t.row_selected } else { t.hover });
        } else if resp.hovered() {
            ui.painter().rect_filled(rect, 0.0, t.hover.gamma_multiply(0.35));
        }
        if t.pro {
            ui.painter().line_segment([pos2(rect.left() + 30.0, rect.top()), pos2(rect.left() + 30.0, rect.bottom())], Stroke::new(1.0, t.separator));
        }
        let (name, on) = match index {
            None => ("Smart Filters".to_string(), sm.filters_enabled),
            Some(i) => (filter_label(&sm.smart_filters[i]), sm.smart_filters[i].visible && sm.filters_enabled),
        };
        let eye = Rect::from_min_size(pos2(rect.left() + 6.0, rect.center().y - 9.0), vec2(18.0, 18.0));
        let eye_resp = ui.interact(eye, ui.id().with(("sf-eye", l.id.0, index)), Sense::click());
        let shown = match index {
            None => sm.filters_enabled,
            Some(i) => sm.smart_filters[i].visible,
        };
        if shown {
            icons::paint(ui, eye, "eye", 12.0, t.icon);
        }
        if eye_resp.clicked() {
            actions.push(match index {
                None => ("layer.smartFilter.disableSmartFilters".into(), json!({"layer": l.id.0})),
                Some(i) => ("layer.smartFilter.setVisible".into(), json!({"layer": l.id.0, "index": i})),
            });
        }
        let x = rect.left() + indent + if index.is_none() { 0.0 } else { 16.0 };
        if index.is_none() {
            let sw = Rect::from_center_size(pos2(x - 12.0, rect.center().y), vec2(14.0, 10.0));
            ui.painter().rect_filled(sw, 1.0, if sm.filter_mask.as_ref().is_some_and(|m| m.enabled) { Color32::from_gray(200) } else { Color32::WHITE });
            ui.painter().rect_stroke(sw, 1.0, Stroke::new(1.0, t.field_border), egui::StrokeKind::Inside);
        }
        ui.painter().text(pos2(x, rect.center().y), Align2::LEFT_CENTER, &name, egui::FontId::proportional(11.5), if on { t.text_dim } else { t.text_faint });
        resp.widget_info(|| egui::WidgetInfo::selected(egui::WidgetType::SelectableLabel, true, selected, &name));
        if resp.clicked() {
            ui.ctx().data_mut(|d| d.insert_temp::<FilterDrag>(selected_key(doc), (l.id.0, index)));
            if !active {
                actions.push(("layer.select".into(), json!({"layer": l.id.0})));
            }
        }
        if let Some(i) = index
            && resp.double_clicked()
        {
            open_editor(app, ui.ctx(), l.id.0, sm, i);
        }
    }
}

/// Smart filter rows being dragged: the smart object and the filter index (`None` = the Smart
/// Filters row, all of them). Also what a click selects.
pub(crate) type FilterDrag = (u64, Option<usize>);

pub(crate) fn drag_key() -> egui::Id {
    egui::Id::new("smart-filter-drag")
}

/// The selected smart filter row of a document.
fn selected_key(doc: photocraft_doc::DocId) -> egui::Id {
    egui::Id::new(("smart-filter-selected", doc))
}

/// While smart filters are dragged: their name by the pointer, and the copy cursor while ⌥ is held.
pub(crate) fn drag_feedback(ctx: &egui::Context, doc: &Document) {
    let Some((from, filter)) = ctx.data(|d| d.get_temp::<FilterDrag>(drag_key())) else { return };
    let Some(p) = ctx.input(|i| i.pointer.interact_pos()) else { return };
    let label = match (filter, doc.layer(LayerId(from)).map(|l| &l.content)) {
        (Some(i), Some(LayerContent::Smart(sm))) => sm.smart_filters.get(i).map_or_else(String::new, filter_label),
        _ => "Smart Filters".to_string(),
    };
    crate::layer_transfer::ghost(ctx, p, &label);
    if ctx.input(|i| i.modifiers.alt) {
        ctx.set_cursor_icon(egui::CursorIcon::Copy);
    }
}

/// The command for dropping the smart filters of `from` on the smart object `target`: they move
/// there, or with ⌥ held on release are copied (Photoshop).
pub(crate) fn drop_action(from: u64, filter: Option<usize>, target: LayerId, copy: bool) -> (String, Value) {
    let mut payload = json!({"from": from, "to": target.0, "copy": copy});
    if let (Some(i), Some(o)) = (filter, payload.as_object_mut()) {
        o.insert("filter".into(), json!(i));
    }
    ("layer.smartFilter.transfer".into(), payload)
}

/// Another smart object's row accepts the smart filters being dragged: outlined while the pointer
/// is over it, dropped on release. Other layers don't take them, as in Photoshop.
pub(crate) fn drop_on_row(ctx: &egui::Context, ui: &egui::Ui, l: &Layer, rect: Rect, actions: &mut Vec<(String, Value)>) {
    let Some((from, filter)) = ctx.data(|d| d.get_temp::<FilterDrag>(drag_key())) else { return };
    let Some(p) = ctx.input(|i| i.pointer.interact_pos()) else { return };
    if from == l.id.0 || !matches!(l.content, LayerContent::Smart(_)) || !rect.contains(p) {
        return;
    }
    let t = Tokens::get(ctx);
    ui.painter().rect_stroke(rect.shrink(1.0), t.radius_sm, Stroke::new(2.0, t.accent), StrokeKind::Inside);
    if ctx.input(|i| i.pointer.any_released()) {
        actions.push(drop_action(from, filter, l.id, ctx.input(|i| i.modifiers.alt)));
    }
}

/// Smart filters dropped on the footer's Delete button: a filter's row deletes that filter, the
/// Smart Filters row clears them all.
pub(crate) fn trash_action((layer, filter): FilterDrag) -> (String, Value) {
    match filter {
        Some(i) => ("layer.smartFilter.delete".into(), json!({"layer": layer, "index": i})),
        None => ("layer.smartFilter.clearSmartFilters".into(), json!({"layer": layer})),
    }
}

/// Re-opens a smart filter's dialog with its recorded parameters; OK runs
/// `layer.smartFilter.setParams` instead of adding another filter (see `dialogs::confirm`).
pub fn open_editor(app: &mut PhotocraftApp, ctx: &egui::Context, layer: u64, sm: &SmartObject, index: usize) {
    let Some(f) = sm.smart_filters.get(index) else { return };
    // Camera Raw has its own full-window dialog rather than a generic parameter form.
    if f.command == photocraft_engine::lens_cmds::RAW {
        if let Err(e) = crate::camera_raw_ui::open_smart_filter(app, ctx, photocraft_doc::LayerId(layer), index) {
            app.ui.status = e;
            app.ui.status_error = true;
        }
        return;
    }
    if !crate::filter_dialog::has_dialog(&f.command) {
        return;
    }
    let Some(id) = crate::filter_dialog::open(app, &f.command) else { return };
    if let Some(d) = app.ui.dialog_mut(id) {
        // The live preview would stack a second copy of the filter on the smart object.
        d.fields.remove("__preview");
        d.fields.insert("__smartFilter".into(), json!({"layer": layer, "index": index}));
        if let Value::Object(p) = &f.params {
            for (k, v) in p {
                if d.fields.contains_key(k) {
                    d.fields.insert(k.clone(), v.clone());
                }
            }
        }
    }
}

/// The command a confirmed filter dialog runs: smart-filter edits update the recorded filter.
pub fn confirm_command(fields: &serde_json::Map<String, Value>, command: String, params: Value) -> (String, Value) {
    match fields.get("__smartFilter") {
        Some(sf) => ("layer.smartFilter.setParams".into(), json!({"layer": sf["layer"], "index": sf["index"], "params": params})),
        None => (command, params),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A pixel layer, a shape layer and a smart object, in that order.
    fn layers() -> [Layer; 3] {
        let mut s = photocraft_engine::Session::new();
        s.execute("file.new", json!({"width": 64, "height": 48})).unwrap();
        // An empty layer can't become a smart object: fill it first.
        s.execute("layer.new.layer", json!({})).unwrap();
        s.execute("edit.fill", json!({"contents": "color", "color": "#336699"})).unwrap();
        let smart = s.execute("layer.smartObjects.convertToSmartObject", json!({})).unwrap()["layer"].as_u64().unwrap();
        let shape = s.execute("shape.create", json!({"kind": "rect", "rect": [8, 8, 40, 24], "fill": "#cc3333"})).unwrap()["layer"].as_u64().unwrap();
        let pixel = s.execute("layer.new.layer", json!({})).unwrap()["layer"].as_u64().unwrap();
        let doc = &s.active().unwrap().doc;
        [pixel, shape, smart].map(|id| doc.layer(photocraft_doc::LayerId(id)).unwrap().clone())
    }

    #[test]
    fn shape_layers_and_smart_objects_carry_a_thumbnail_badge() {
        let [pixel, shape, smart] = layers();
        assert_eq!(thumb_badge_kind(&pixel), None);
        assert_eq!(thumb_badge_kind(&shape), Some(ThumbBadge::Shape), "#4584");
        assert_eq!(thumb_badge_kind(&smart), Some(ThumbBadge::Smart));
    }

    /// The rects `thumb_badge` fills for `l` over `thumb`, with their colours.
    fn filled(l: &Layer, thumb: Rect) -> Vec<(Rect, Color32)> {
        let l = l.clone();
        let mut h = egui_kittest::Harness::new_ui(move |ui| thumb_badge(ui, &l, thumb));
        h.run();
        let badge = thumb_badge_rect(thumb).expand(0.5);
        let rects = h.output().shapes.iter().filter_map(|s| match &s.shape {
            egui::Shape::Rect(r) if badge.contains_rect(r.rect) && r.fill != Color32::TRANSPARENT => Some((r.rect, r.fill)),
            _ => None,
        });
        rects.collect()
    }

    /// #4584: a shape layer's thumbnail shows a path with four anchor points in its corner.
    #[test]
    fn shape_badge_paints_a_path_with_an_anchor_on_each_corner() {
        let [pixel, shape, smart] = layers();
        let thumb = Rect::from_min_size(pos2(20.0, 20.0), vec2(34.0, 34.0));
        let t = Tokens::get(&egui::Context::default());
        let frame = thumb_badge_rect(thumb);
        assert!(frame.right() > thumb.right() && frame.bottom() > thumb.bottom(), "over the bottom-right corner");
        assert!(filled(&pixel, thumb).is_empty(), "a pixel layer has no badge");

        let drawn = filled(&shape, thumb);
        assert!(drawn.contains(&(frame, t.field)), "the badge's frame");
        let anchors: Vec<Rect> = drawn.iter().filter(|(_, c)| *c == t.icon).map(|(r, _)| *r).collect();
        let path = frame.shrink(SHAPE_PATH_INSET);
        let corners = [path.left_top(), path.right_top(), path.right_bottom(), path.left_bottom()];
        assert_eq!(anchors.iter().map(Rect::center).collect::<Vec<_>>(), corners);
        for a in &anchors {
            assert_eq!(a.size(), vec2(SHAPE_ANCHOR, SHAPE_ANCHOR));
            assert!(frame.shrink(1.0).contains_rect(*a), "inside the frame's border");
        }

        // The smart-object badge keeps its frame and draws no anchors.
        let drawn = filled(&smart, thumb);
        assert!(drawn.contains(&(frame, t.field)));
        assert!(drawn.iter().all(|(_, c)| *c != t.icon));
    }
}
