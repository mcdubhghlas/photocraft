#include "type_overlay.h"
#include "engine/channel_display.h"
pc_ui_channel_overlay pc_ui_type_mask_overlay(const pc_engine_session *session, const pc_ui_state *ui)
{
    pc_ui_channel_overlay overlay = {0};
    if (!session || !ui || !ui->has_text_edit || !ui->text_edit.mask) {
        return overlay;
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state) {
        return overlay;
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (ui->text_edit.document && ui->text_edit.document != doc->id) {
        return overlay;
    }
    const pc_layer *layer = pc_document_layer(doc, ui->text_edit.layer);
    if (!layer || layer->content.kind != PC_LAYER_TEXT) {
        return overlay;
    }
    overlay.active = true;
    overlay.key = layer->id;
    overlay.surface = layer->content.u.text.has_cache ? &layer->content.u.text.cache : NULL;
    pc_engine_quick_mask_display(session, overlay.color, &overlay.opacity);
    return overlay;
}
