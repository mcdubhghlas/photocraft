/* Character and paragraph styles <-> the PSD text engine's style sheets (port of
 * crates/io/src/text_styles_map.rs; the sheets themselves are src/text/psd.h). Import merges the
 * named sheets of every type layer into the document's text styles by name and sets the runs'
 * style references; export rewrites each type layer's sheets from the document's styles. Files
 * without named styles are left as they are (byte-exact). */
#include "io/text_styles_map.h"

#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "text/psd.h"

/* (sheet index, style id) pairs: HashMap<usize, u32> with the last insert winning. */
typedef struct idmap {
    size_t *ix;
    uint32_t *id;
    size_t n;
} idmap;

static bool idmap_get(const idmap *m, size_t ix, uint32_t *out)
{
    for (size_t i = m->n; i-- > 0;) {
        if (m->ix[i] == ix) {
            *out = m->id[i];
            return true;
        }
    }
    return false;
}

static void idmap_fini(idmap *m)
{
    free(m->ix);
    free(m->id);
    memset(m, 0, sizeof *m);
}

static pc_status idmap_alloc(idmap *m, size_t cap, pc_error *err)
{
    memset(m, 0, sizeof *m);
    m->ix = pc_malloc_array(cap > 0 ? cap : 1, sizeof *m->ix);
    m->id = pc_malloc_array(cap > 0 ? cap : 1, sizeof *m->id);
    if (m->ix == NULL || m->id == NULL) {
        idmap_fini(m);
        return pc_io_nomem(err, "importing text styles");
    }
    return PC_OK;
}

/* The id of the last style named name (HashMap<String, u32> collected from the list). */
static bool char_id(const pc_text_styles *s, const pc_str *name, uint32_t *out)
{
    for (size_t i = s->ncharacter; i-- > 0;) {
        if (pc_str_eq(&s->character[i].name, name)) {
            *out = s->character[i].id;
            return true;
        }
    }
    return false;
}

static bool para_id(const pc_text_styles *s, const pc_str *name, uint32_t *out)
{
    for (size_t i = s->nparagraph; i-- > 0;) {
        if (pc_str_eq(&s->paragraph[i].name, name)) {
            *out = s->paragraph[i].id;
            return true;
        }
    }
    return false;
}

static void drop_postscript(pc_style_attrs *a)
{
    yyjson_mut_val *root = pc_json_value_root(a);
    if (root != NULL && yyjson_mut_is_obj(root)) {
        (void)yyjson_mut_obj_remove_key(root, "postscript_name");
    }
}

static pc_status warn_exhausted(pc_io_warnings *w, const char *what, const pc_str *name, pc_error *err)
{
    size_t cap = name->len * 10 + 16;
    char *q = malloc(cap);
    pc_status st;
    if (q == NULL) {
        return pc_io_nomem(err, "importing text styles");
    }
    pc_utf8_debug_quote((const uint8_t *)pc_str_c(name), name->len, q, cap);
    st = pc_io_warn(w, err, "%s style %s was not imported: style id space exhausted", what, q);
    free(q);
    return st;
}

static pc_status add_char_style(pc_text_styles *styles, const pc_psd_char_sheet *sh,
                                const pc_psd_style_sheets *sheets, uint32_t id, pc_error *err)
{
    pc_character_style_def def;
    pc_character_style_def *v;
    pc_status st;
    memset(&def, 0, sizeof def);
    def.id = id;
    st = pc_char_style_diff_attrs(&sh->style, &sheets->normal_char, &def.attrs, err);
    if (st == PC_OK) {
        drop_postscript(&def.attrs);
        st = pc_str_clone(&sh->name, &def.name, err);
    }
    if (st == PC_OK) {
        v = realloc(styles->character, (styles->ncharacter + 1) * sizeof *v);
        if (v == NULL) {
            st = pc_io_nomem(err, "importing text styles");
        } else {
            styles->character = v;
            v[styles->ncharacter++] = def;
            return PC_OK;
        }
    }
    pc_character_style_def_fini(&def);
    return st;
}

static pc_status add_para_style(pc_text_styles *styles, const pc_psd_para_sheet *sh,
                                const pc_psd_style_sheets *sheets, uint32_t id, pc_error *err)
{
    pc_paragraph_style_def def;
    pc_paragraph_style_def *v;
    pc_status st = PC_OK;
    memset(&def, 0, sizeof def);
    def.id = id;
    if (sh->has_char) {
        st = pc_char_style_diff_attrs(&sh->chr, &sheets->normal_char, &def.char_attrs, err);
        if (st == PC_OK) {
            drop_postscript(&def.char_attrs);
        }
    }
    if (st == PC_OK) {
        st = pc_paragraph_style_diff_attrs(&sh->style, &sheets->normal_para, &def.para_attrs, err);
    }
    if (st == PC_OK) {
        st = pc_str_clone(&sh->name, &def.name, err);
    }
    if (st == PC_OK) {
        v = realloc(styles->paragraph, (styles->nparagraph + 1) * sizeof *v);
        if (v == NULL) {
            st = pc_io_nomem(err, "importing text styles");
        } else {
            styles->paragraph = v;
            v[styles->nparagraph++] = def;
            return PC_OK;
        }
    }
    pc_paragraph_style_def_fini(&def);
    return st;
}

static void set_ref(bool *has, uint32_t *id, const pc_opt_index *ix, const idmap *m)
{
    uint32_t v = 0;
    *has = ix->has && idmap_get(m, ix->v, &v);
    *id = *has ? v : 0;
}

static void link(pc_text_layer *t, const pc_psd_style_sheets *sh, const idmap *cmap, const idmap *pmap)
{
    if (sh->nrun_char_lens == sh->nrun_char && sh->nrun_char > 0) {
        /* Each model run takes the sheet of the StyleRun entry its first character is in. */
        size_t off = 0;
        size_t i = 0;
        size_t end = sh->run_char_lens[0];
        for (size_t r = 0; r < t->nruns; r++) {
            pc_char_style *s = &t->runs[r].style;
            while (off >= end && i + 1 < sh->nrun_char_lens) {
                i++;
                end = end > SIZE_MAX - sh->run_char_lens[i] ? SIZE_MAX : end + sh->run_char_lens[i];
            }
            set_ref(&s->has_style_sheet, &s->style_sheet, &sh->run_char[i], cmap);
            off = off > SIZE_MAX - t->runs[r].len ? SIZE_MAX : off + t->runs[r].len;
        }
    } else if (t->nruns == sh->nrun_char) {
        for (size_t r = 0; r < t->nruns; r++) {
            pc_char_style *s = &t->runs[r].style;
            set_ref(&s->has_style_sheet, &s->style_sheet, &sh->run_char[r], cmap);
        }
    }
    if (t->nparagraphs == sh->nrun_para) {
        for (size_t r = 0; r < t->nparagraphs; r++) {
            pc_paragraph_style *s = &t->paragraphs[r].style;
            set_ref(&s->has_style_sheet, &s->style_sheet, &sh->run_para[r], pmap);
        }
    }
}

/* One type layer: merge its named sheets into styles and link its runs. */
static pc_status import_layer(pc_text_layer *t, float dpi, pc_text_styles *styles, pc_io_warnings *warnings,
                              pc_error *err)
{
    const uint8_t *raw = pc_blob_data(t->psd_raw);
    size_t len = pc_blob_len(t->psd_raw);
    bool named = false;
    pc_psd_style_sheets sh;
    idmap cmap;
    idmap pmap;
    pc_status st;
    if (t->psd_raw == NULL) {
        return PC_OK;
    }
    st = pc_text_has_named_sheets(raw != NULL ? raw : (const uint8_t *)"", len, &named, err);
    if (st == PC_ERR_NOMEM) {
        return st;
    }
    if (st != PC_OK || !named) {
        return PC_OK;
    }
    st = pc_text_read_style_sheets(raw, len, dpi, &sh, err);
    if (st == PC_ERR_NOMEM) {
        return st;
    }
    if (st != PC_OK) {
        return PC_OK;
    }
    st = idmap_alloc(&cmap, sh.nchar_sheets, err);
    if (st == PC_OK) {
        st = idmap_alloc(&pmap, sh.npara_sheets, err);
        if (st != PC_OK) {
            idmap_fini(&cmap);
        }
    }
    if (st != PC_OK) {
        pc_psd_style_sheets_fini(&sh);
        return st;
    }
    for (size_t k = 0; k < sh.nchar_sheets && st == PC_OK; k++) {
        const pc_psd_char_sheet *c = &sh.char_sheets[k];
        uint32_t id = 0;
        if (!char_id(styles, &c->name, &id)) {
            if (!pc_text_styles_next_char_id(styles, &id)) {
                st = warnings != NULL ? warn_exhausted(warnings, "character", &c->name, err) : PC_OK;
                continue;
            }
            st = add_char_style(styles, c, &sh, id, err);
            if (st != PC_OK) {
                break;
            }
        }
        cmap.ix[cmap.n] = c->index;
        cmap.id[cmap.n++] = id;
    }
    for (size_t k = 0; k < sh.npara_sheets && st == PC_OK; k++) {
        const pc_psd_para_sheet *p = &sh.para_sheets[k];
        uint32_t id = 0;
        if (!para_id(styles, &p->name, &id)) {
            if (!pc_text_styles_next_para_id(styles, &id)) {
                st = warnings != NULL ? warn_exhausted(warnings, "paragraph", &p->name, err) : PC_OK;
                continue;
            }
            st = add_para_style(styles, p, &sh, id, err);
            if (st != PC_OK) {
                break;
            }
        }
        pmap.ix[pmap.n] = p->index;
        pmap.id[pmap.n++] = id;
    }
    if (st == PC_OK) {
        link(t, &sh, &cmap, &pmap);
    }
    idmap_fini(&cmap);
    idmap_fini(&pmap);
    pc_psd_style_sheets_fini(&sh);
    return st;
}

pc_status pc_io_text_styles_import(pc_document *doc, pc_io_warnings *warnings, pc_error *err)
{
    pc_walk w;
    pc_layer_id *ids = NULL;
    size_t n = 0;
    pc_status st = pc_document_walk(doc, &w, err);
    if (st != PC_OK) {
        return st;
    }
    if (w.len > 0) {
        ids = pc_malloc_array(w.len, sizeof *ids);
        if (ids == NULL) {
            pc_walk_fini(&w);
            return pc_io_nomem(err, "importing text styles");
        }
    }
    for (size_t i = 0; i < w.len; i++) {
        if (w.items[i].layer->content.kind == PC_LAYER_TEXT) {
            ids[n++] = w.items[i].layer->id;
        }
    }
    pc_walk_fini(&w);
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        pc_layer *l = pc_document_layer_mut(doc, ids[i]);
        if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        st = import_layer(&l->content.u.text, doc->resolution_dpi, &doc->text_styles, warnings, err);
    }
    free(ids);
    return st;
}

pc_status pc_io_export_tysh(const uint8_t *data, size_t len, const pc_text_layer *t,
                            const pc_text_styles *styles, float dpi, bool *has, pc_buf *out, pc_error *err)
{
    bool named = styles->ncharacter > 0 || styles->nparagraph > 0;
    pc_psd_bytes bytes;
    pc_buf b = PC_BUF_INIT;
    pc_status st;
    if (data == NULL) {
        data = (const uint8_t *)"";
        len = 0;
    }
    if (!named) {
        bool sheets = false;
        st = pc_text_has_named_sheets(data, len, &sheets, err);
        if (st == PC_ERR_NOMEM) {
            return st;
        }
        if (st != PC_OK || !sheets) {
            *has = false;
            return PC_OK;
        }
    }
    st = pc_text_write_style_sheets(data, len, t, styles, dpi, &bytes, err);
    if (st == PC_ERR_NOMEM) {
        return st;
    }
    if (st != PC_OK) {
        *has = false;
        return PC_OK;
    }
    st = pc_buf_append(&b, bytes.data, bytes.len, err);
    pc_psd_bytes_fini(&bytes);
    if (st != PC_OK) {
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    *has = true;
    return PC_OK;
}
