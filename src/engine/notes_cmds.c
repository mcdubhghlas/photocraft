/* Note tool annotations (Window > Notes, File > Import > Notes...). Notes are document data
 * (pc_note), saved in .pcraft and as the PSD Anno block; each change is one history step.
 * Port of crates/engine/src/notes_cmds.rs (E12). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/num.h"
#include "base/str.h"
#include "color/color.h"
#include "engine_internal.h"

/* PDF-style date (D:YYYYMMDDhhmmssZ) as Photoshop stores in notes. Mirrors the reference, which
 * takes analysis_cmds::now_iso() (ISO 8601 UTC) and keeps its digits. */
static void pdf_date(pc_str *out)
{
    time_t secs = time(NULL);
    struct tm tmv;
    char iso[32];
    if (gmtime_r(&secs, &tmv) == NULL) {
        (void)pc_str_set_c(out, "", NULL);
        return;
    }
    (void)strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    if (strlen(iso) < 19) {
        (void)pc_str_set_c(out, "", NULL);
        return;
    }
    char buf[2 + 14 + 1 + 1];
    size_t k = 0;
    buf[k++] = 'D';
    buf[k++] = ':';
    for (size_t i = 0; i < 19 && k < sizeof buf - 2; i++) {
        if (iso[i] >= '0' && iso[i] <= '9') {
            buf[k++] = iso[i];
        }
    }
    buf[k++] = 'Z';
    buf[k] = '\0';
    (void)pc_str_set_c(out, buf, NULL);
}

static pc_status notes_has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return PC_OK;
}

static pc_status notes_has_notes(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (pc_doc_arc_get(d->doc)->nnotes == 0) {
        return pc_fail(reason, PC_ERR_INVALID, "the document has no notes");
    }
    return PC_OK;
}

/* note_json(i, n) into a mutable value. */
static yyjson_mut_val *note_json(yyjson_mut_doc *d, size_t i, const pc_note *n)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, o, "index", i);
    yyjson_mut_obj_add_strcpy(d, o, "author", pc_str_c(&n->author));
    yyjson_mut_obj_add_strcpy(d, o, "text", pc_str_c(&n->text));
    float rgb[3];
    pc_color_to_rgb(&n->color, rgb);
    yyjson_mut_val *col = yyjson_mut_arr(d);
    for (size_t k = 0; k < 3; k++) {
        yyjson_mut_arr_add_real(d, col, (double)rgb[k]);
    }
    yyjson_mut_obj_add_val(d, o, "color", col);
    yyjson_mut_val *pos = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, pos, n->position[0]);
    yyjson_mut_arr_add_real(d, pos, n->position[1]);
    yyjson_mut_obj_add_val(d, o, "position", pos);
    yyjson_mut_obj_add_bool(d, o, "open", n->open);
    yyjson_mut_obj_add_strcpy(d, o, "modified", pc_str_c(&n->modified));
    return o;
}

/* {"notes":[...]} into a mutable doc rooted at an object. */
static yyjson_mut_val *list_json(yyjson_mut_doc *d, const pc_document *doc)
{
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < doc->nnotes; i++) {
        yyjson_mut_arr_append(arr, note_json(d, i, &doc->notes[i]));
    }
    yyjson_mut_obj_add_val(d, root, "notes", arr);
    return root;
}

/* Colour override from params (reference color_of): absent -> keep, present -> pale-yellow default. */
static bool color_of(yyjson_val *p, pc_color *out)
{
    if (p == NULL || pc_json_get(p, "color") == NULL) {
        return false;
    }
    static const float def[4] = {1.0f, 1.0f, 0.51f, 1.0f};
    float c[4];
    pc_engine_param_color(p, "color", def, c);
    *out = pc_color_rgb(c[0], c[1], c[2]);
    return true;
}

static pc_status index_of(const pc_engine_session *s, yyjson_val *p, const char *cmd, size_t *out,
                          pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    size_t n = pc_doc_arc_get(d->doc)->nnotes;
    uint64_t i = 0;
    yyjson_val *iv = pc_json_get(p, "index");
    if (iv == NULL || !pc_json_as_u64(iv, &i)) {
        return pc_engine_err_bad_params(err, cmd, "pass `index`");
    }
    if (i >= (uint64_t)n) {
        return pc_engine_err_bad_params(err, cmd, "no note %llu (the document has %zu)",
                                        (unsigned long long)i, n);
    }
    *out = (size_t)i;
    return PC_OK;
}

/* ---- notes.add --------------------------------------------------------------------------- */

typedef struct {
    pc_note note;
    size_t index; /* out */
} add_ctx;

static pc_status edit_add(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                          pc_error *err)
{
    (void)has_active;
    (void)active;
    add_ctx *a = ctx;
    size_t need = doc->nnotes + 1;
    if (need == 0 || need > SIZE_MAX / sizeof *doc->notes) {
        return pc_fail(err, PC_ERR_LIMIT, "notes.add");
    }
    pc_note *grown = realloc(doc->notes, need * sizeof *doc->notes);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "notes.add");
    }
    doc->notes = grown;
    /* move the prepared note in (ownership transferred) */
    doc->notes[doc->nnotes] = a->note;
    a->note = (pc_note){0};
    a->index = doc->nnotes;
    doc->nnotes++;
    return PC_OK;
}

static pc_status run_add(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "notes.add";
    double x = 0, y = 0;
    yyjson_val *xv = pc_json_get(p, "x"), *yv = pc_json_get(p, "y");
    if (xv == NULL || !pc_json_as_f64(xv, &x) || yv == NULL || !pc_json_as_f64(yv, &y)) {
        return pc_engine_err_bad_params(err, CMD, "pass `x` and `y`");
    }
    if (!isfinite(x) || !isfinite(y)) {
        return pc_engine_err_bad_params(err, CMD, "coordinates must be finite");
    }
    add_ctx a;
    memset(&a, 0, sizeof a);
    pc_note_default(&a.note);
    size_t slen = 0;
    const char *sv;
    yyjson_val *av = pc_json_get(p, "author");
    if (av != NULL && (sv = pc_json_as_str(av, &slen)) != NULL) {
        if (pc_str_set(&a.note.author, sv, slen, err) != PC_OK) {
            pc_note_fini(&a.note);
            return PC_ERR_NOMEM;
        }
    }
    yyjson_val *tv = pc_json_get(p, "text");
    if (tv != NULL && (sv = pc_json_as_str(tv, &slen)) != NULL) {
        if (pc_str_set(&a.note.text, sv, slen, err) != PC_OK) {
            pc_note_fini(&a.note);
            return PC_ERR_NOMEM;
        }
    }
    a.note.position[0] = x;
    a.note.position[1] = y;
    a.note.popup[0] = x + 20.0;
    a.note.popup[1] = y + 20.0;
    a.note.popup[2] = x + 260.0;
    a.note.popup[3] = y + 160.0;
    bool open = true;
    yyjson_val *ov = pc_json_get(p, "open");
    if (ov != NULL) {
        (void)pc_json_as_bool(ov, &open);
    }
    a.note.open = open;
    pdf_date(&a.note.modified);
    pc_color c;
    if (color_of(p, &c)) {
        a.note.color = c;
    }
    pc_status rc = pc_engine_session_edit(s, "New Note", edit_add, &a, err);
    if (rc != PC_OK) {
        pc_note_fini(&a.note);
        return rc;
    }
    /* edit_add moved the note in; a.note is empty now. */
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (a.index >= doc->nnotes) {
        return pc_engine_err_no_document(err);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "index", a.index);
    yyjson_mut_obj_add_val(rd, root, "note", note_json(rd, a.index, &doc->notes[a.index]));
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- notes.set --------------------------------------------------------------------------- */

typedef struct {
    size_t index;
    pc_note note; /* the new value */
} set_ctx;

static pc_status edit_set(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                          pc_error *err)
{
    (void)has_active;
    (void)active;
    set_ctx *c = ctx;
    if (c->index >= doc->nnotes) {
        return pc_engine_err_no_layer(err, 0);
    }
    pc_note_fini(&doc->notes[c->index]);
    doc->notes[c->index] = c->note;
    c->note = (pc_note){0};
    return PC_OK;
}

static pc_status run_set(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "notes.set";
    size_t i = 0;
    pc_status rc = index_of(s, p, CMD, &i, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_note *orig = &doc->notes[i];
    pc_note n;
    if (pc_note_clone(orig, &n, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    size_t slen = 0;
    const char *sv;
    yyjson_val *tv = pc_json_get(p, "text");
    if (tv != NULL && (sv = pc_json_as_str(tv, &slen)) != NULL) {
        (void)pc_str_set(&n.text, sv, slen, NULL);
    }
    yyjson_val *av = pc_json_get(p, "author");
    if (av != NULL && (sv = pc_json_as_str(av, &slen)) != NULL) {
        (void)pc_str_set(&n.author, sv, slen, NULL);
    }
    pc_color c;
    if (color_of(p, &c)) {
        n.color = c;
    }
    bool ob;
    yyjson_val *ov = pc_json_get(p, "open");
    if (ov != NULL && pc_json_as_bool(ov, &ob)) {
        n.open = ob;
    }
    yyjson_val *xv = pc_json_get(p, "x"), *yv = pc_json_get(p, "y");
    double x = 0, y = 0;
    bool hx = xv != NULL && pc_json_as_f64(xv, &x);
    bool hy = yv != NULL && pc_json_as_f64(yv, &y);
    if (xv != NULL || yv != NULL) {
        double nx = hx ? x : n.position[0];
        double ny = hy ? y : n.position[1];
        if (!isfinite(nx) || !isfinite(ny)) {
            pc_note_fini(&n);
            return pc_engine_err_bad_params(err, CMD, "coordinates must be finite");
        }
        double dx = nx - n.position[0];
        double dy = ny - n.position[1];
        n.position[0] = nx;
        n.position[1] = ny;
        n.popup[0] += dx;
        n.popup[1] += dy;
        n.popup[2] += dx;
        n.popup[3] += dy;
    }
    bool pos_changed = n.position[0] != orig->position[0] || n.position[1] != orig->position[1];
    if (pc_note_eq(&n, orig)) {
        yyjson_mut_doc *rd = NULL;
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            pc_note_fini(&n);
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, root, "index", i);
        yyjson_mut_obj_add_val(rd, root, "note", note_json(rd, i, &n));
        yyjson_mut_obj_add_bool(rd, root, "changed", false);
        yyjson_mut_doc_set_root(rd, root);
        pc_note_fini(&n);
        return PC_OK;
    }
    if (!pc_str_eq(&n.text, &orig->text) || !pc_str_eq(&n.author, &orig->author)) {
        pdf_date(&n.modified);
    }
    const char *label = pos_changed ? "Move Note" : "Edit Note";
    /* build result before moving the note into the document */
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_note_fini(&n);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "index", i);
    yyjson_mut_obj_add_val(rd, root, "note", note_json(rd, i, &n));
    yyjson_mut_obj_add_bool(rd, root, "changed", true);
    set_ctx sc = {.index = i, .note = n};
    rc = pc_engine_session_edit(s, label, edit_set, &sc, err);
    if (rc != PC_OK) {
        pc_note_fini(&sc.note);
        pc_json_value_fini(out);
        return rc;
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- notes.delete ------------------------------------------------------------------------ */

typedef struct {
    bool all;
    size_t index;
} del_ctx;

static pc_status edit_delete(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                             pc_error *err)
{
    (void)has_active;
    (void)active;
    (void)err;
    del_ctx *c = ctx;
    if (c->all) {
        for (size_t k = 0; k < doc->nnotes; k++) {
            pc_note_fini(&doc->notes[k]);
        }
        doc->nnotes = 0;
        return PC_OK;
    }
    if (c->index >= doc->nnotes) {
        return PC_OK;
    }
    pc_note_fini(&doc->notes[c->index]);
    memmove(&doc->notes[c->index], &doc->notes[c->index + 1],
            (doc->nnotes - c->index - 1) * sizeof *doc->notes);
    doc->nnotes--;
    return PC_OK;
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    bool all = false;
    yyjson_val *av = pc_json_get(p, "all");
    if (av != NULL && pc_json_as_bool(av, &all) && all) {
        size_t n = 0;
        const pc_engine_doc_state *d = pc_engine_session_active(s);
        if (d != NULL) {
            n = pc_doc_arc_get(d->doc)->nnotes;
        }
        del_ctx c = {.all = true, .index = 0};
        pc_status rc = pc_engine_session_edit(s, "Delete All Notes", edit_delete, &c, err);
        if (rc != PC_OK) {
            return rc;
        }
        yyjson_mut_doc *rd = NULL;
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, root, "deleted", n);
        yyjson_mut_obj_add_uint(rd, root, "remaining", 0);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    size_t i = 0;
    pc_status rc = index_of(s, p, "notes.delete", &i, err);
    if (rc != PC_OK) {
        return rc;
    }
    del_ctx c = {.all = false, .index = i};
    rc = pc_engine_session_edit(s, "Delete Note", edit_delete, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t remaining = 0;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL) {
        remaining = pc_doc_arc_get(d->doc)->nnotes;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "deleted", 1);
    yyjson_mut_obj_add_uint(rd, root, "remaining", remaining);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(rd, list_json(rd, pc_doc_arc_get(d->doc)));
    return PC_OK;
}

/* ---- File > Import > Notes ---------------------------------------------------------------- */

typedef struct {
    pc_note *notes;
    size_t n;
} import_ctx;

static pc_status edit_import(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                             pc_error *err)
{
    (void)has_active;
    (void)active;
    import_ctx *c = ctx;
    if (c->n == 0) {
        return PC_OK;
    }
    size_t total;
    if (!pc_size_add(doc->nnotes, c->n, &total) || total == 0 ||
        total > SIZE_MAX / sizeof *doc->notes) {
        return pc_fail(err, PC_ERR_LIMIT, "import notes");
    }
    pc_note *grown = realloc(doc->notes, total * sizeof *doc->notes);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "import notes");
    }
    doc->notes = grown;
    for (size_t k = 0; k < c->n; k++) {
        doc->notes[doc->nnotes + k] = c->notes[k];
    }
    doc->nnotes += c->n;
    /* ownership moved; caller frees only the array shell */
    c->n = 0;
    return PC_OK;
}

static double clampd(double v, double lo, double hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

pc_status pc_engine_import_notes_from(pc_engine_session *s, const char *name, const uint8_t *bytes,
                                      size_t len, pc_json_value *out, pc_error *err)
{
    pc_document src;
    pc_status rc = pc_engine_import(name, bytes, len, &src, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (src.nnotes == 0) {
        pc_document_fini(&src);
        return pc_engine_err_other(err, "%s has no notes", name);
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        pc_document_fini(&src);
        return pc_engine_err_no_document(err);
    }
    const pc_document *dst = pc_doc_arc_get(d->doc);
    double w = (double)dst->size.width, h = (double)dst->size.height;
    import_ctx c = {.notes = src.notes, .n = src.nnotes};
    for (size_t k = 0; k < c.n; k++) {
        pc_note *n = &c.notes[k];
        double nx = clampd(n->position[0], 0.0, (w - 16.0) > 0 ? w - 16.0 : 0.0);
        double ny = clampd(n->position[1], 0.0, (h - 20.0) > 0 ? h - 20.0 : 0.0);
        double dx = nx - n->position[0];
        double dy = ny - n->position[1];
        n->position[0] = nx;
        n->position[1] = ny;
        n->popup[0] += dx;
        n->popup[1] += dy;
        n->popup[2] += dx;
        n->popup[3] += dy;
    }
    size_t count = c.n;
    /* detach notes array from src so its fini won't free them (moved into dst by edit) */
    src.notes = NULL;
    src.nnotes = 0;
    rc = pc_engine_session_edit(s, "Import Notes", edit_import, &c, err);
    if (rc != PC_OK) {
        for (size_t k = 0; k < c.n; k++) {
            pc_note_fini(&c.notes[k]);
        }
        free(c.notes);
        pc_document_fini(&src);
        return rc;
    }
    free(c.notes); /* notes moved into dst; only the shell remains */
    pc_document_fini(&src);
    d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = list_json(rd, pc_doc_arc_get(d->doc));
    yyjson_mut_obj_add_uint(rd, root, "imported", count);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_import(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "file.import.notes";
    size_t plen = 0;
    const char *path = NULL;
    yyjson_val *pv = pc_json_get(p, "path");
    if (pv != NULL) {
        path = pc_json_as_str(pv, &plen);
    }
    if (path == NULL) {
        return pc_engine_err_bad_params(err, CMD, "pass `path` (.psd, .psb or .pcraft)");
    }
    pc_buf bytes = PC_BUF_INIT;
    pc_status rc = pc_engine_read_file(path, &bytes, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* file name (basename) for error text */
    const char *slash = strrchr(path, '/');
    const char *name = slash != NULL ? slash + 1 : path;
    rc = pc_engine_import_notes_from(s, name, bytes.data, bytes.len, out, err);
    pc_buf_fini(&bytes);
    return rc;
}

static const char *const MENU_FILE_IMPORT[] = {"File", "Import"};

static const pc_engine_command SPECS[] = {
    {"notes.add", "New Note", NULL, 0, NULL,
     "{\"x\":px,\"y\":px,\"text\":str=\"\",\"author\":str=\"\",\"color\":\"#rrggbb\"|[r,g,b]=pale "
     "yellow,\"open\":bool=true}",
     notes_has_doc, run_add, true},
    {"notes.set", "Edit Note", NULL, 0, NULL,
     "{\"index\":n,\"text\":str?,\"author\":str?,\"color\":\"#rrggbb\"|[r,g,b]?,\"x\":px?,\"y\":px?,"
     "\"open\":bool?}",
     notes_has_notes, run_set, true},
    {"notes.delete", "Delete Note", NULL, 0, NULL, "{\"index\":n}|{\"all\":true}", notes_has_notes,
     run_delete, true},
    {"notes.list", "Notes", NULL, 0, NULL,
     "{} \xE2\x86\x92 notes (index, author, text, colour, position, open, modified)", notes_has_doc,
     run_list, false},
    {"file.import.notes", "Notes\xE2\x80\xA6", MENU_FILE_IMPORT, 2, NULL,
     "{\"path\":\".psd|.psb|.pcraft with notes\"}", notes_has_doc, run_import, true},
};

const pc_engine_command *pc_engine_notes_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
