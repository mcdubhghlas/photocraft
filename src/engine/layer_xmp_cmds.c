/* layer.xmpMetadata (port of crates/engine/src/layer_xmp_cmds.rs): a layer's own XMP packet, as
 * Photoshop scripts reach it through layer.xmpMetadata.rawData. Saved in PSD/PSB/layered TIFF and
 * .pcraft, and copied when the layer is duplicated.
 *
 * Never crashes (REWRITE_PLAN.md rule 9): the packet is size-capped and checked before it is
 * stored. The reference validates well-formed XML with roxmltree; the C tree has no XML parser, so
 * this does the safety checks (size, no NUL) and a lightweight root-element check (the root's local
 * name must be x:xmpmeta or rdf:RDF, no DTD) rather than full XML well-formedness. */
#include <stddef.h>
#include <string.h>

#include "base/json.h"
#include "doc/doc.h"
#include "geom/geom.h"

#include "engine_internal.h"
#include "layer_xmp_cmds.h"

#define CMD "layer.xmpMetadata"

/* Largest packet accepted, in UTF-8 bytes. */
#define MAX_LAYER_XMP (4u << 20)

/* Skip ASCII whitespace. */
static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    return p;
}

/* `raw` if it looks like an XMP packet: no NUL, within the size cap, and a root element whose
 * local name is "xmpmeta" or "RDF", with no DTD. Returns PC_OK or a bad-params error. */
static pc_status check_packet(const char *raw, size_t len, pc_error *err)
{
    if (len > MAX_LAYER_XMP) {
        return pc_engine_err_bad_params(err, CMD, "`rawData` is %zu bytes; the most a layer holds is %u",
                                        len, (unsigned)MAX_LAYER_XMP);
    }
    if (memchr(raw, '\0', len) != NULL) {
        return pc_engine_err_bad_params(err, CMD, "`rawData` can't contain NUL characters");
    }
    const char *p = raw;
    const char *end = raw + len;
    /* Skip leading whitespace, XML declarations / processing instructions and comments to the
     * first real element; reject a DTD. */
    for (;;) {
        p = skip_ws(p, end);
        if (p >= end || *p != '<') {
            return pc_engine_err_bad_params(err, CMD, "`rawData` isn't well-formed XML");
        }
        if (p + 1 < end && p[1] == '?') { /* <?xml ... ?> or other PI */
            const char *close = NULL;
            for (const char *q = p + 2; q + 1 < end; q++) {
                if (q[0] == '?' && q[1] == '>') {
                    close = q + 2;
                    break;
                }
            }
            if (close == NULL) {
                return pc_engine_err_bad_params(err, CMD, "`rawData` isn't well-formed XML");
            }
            p = close;
            continue;
        }
        if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') { /* <!-- comment --> */
            const char *close = NULL;
            for (const char *q = p + 4; q + 2 < end; q++) {
                if (q[0] == '-' && q[1] == '-' && q[2] == '>') {
                    close = q + 3;
                    break;
                }
            }
            if (close == NULL) {
                return pc_engine_err_bad_params(err, CMD, "`rawData` isn't well-formed XML");
            }
            p = close;
            continue;
        }
        if (p + 1 < end && p[1] == '!') { /* <!DOCTYPE ...> or other declaration: refused */
            return pc_engine_err_bad_params(err, CMD, "`rawData` must be an XMP packet (no DTD)");
        }
        break; /* a real element start */
    }
    /* Read the element name, strip any namespace prefix, compare its local name. */
    const char *name = p + 1;
    const char *q = name;
    while (q < end && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n' && *q != '>' && *q != '/') {
        q++;
    }
    const char *local = name;
    for (const char *r = name; r < q; r++) {
        if (*r == ':') {
            local = r + 1;
        }
    }
    size_t llen = (size_t)(q - local);
    bool xmp = (llen == 7 && memcmp(local, "xmpmeta", 7) == 0) || (llen == 3 && memcmp(local, "RDF", 3) == 0);
    if (!xmp) {
        return pc_engine_err_bad_params(err, CMD,
                                        "`rawData` must be an XMP packet: its root element is x:xmpmeta or rdf:RDF");
    }
    return PC_OK;
}

static pc_status xmp_enabled(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_active_layer_of(s, reason) == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    return PC_OK;
}

struct xmp_ctx {
    pc_layer_id id;
    bool set;       /* true when applying a new value */
    bool remove;    /* true when the new value is null (remove) */
    const char *raw; /* new packet bytes (borrowed from params), when set && !remove */
    size_t raw_len;
};

static pc_status xmp_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct xmp_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    if (c->remove) {
        pc_str_fini(&l->xmp);
        l->has_xmp = false;
        return PC_OK;
    }
    pc_str v = PC_STR_INIT;
    pc_status rc = pc_str_set(&v, c->raw, c->raw_len, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_str_fini(&l->xmp);
    l->xmp = v;
    l->has_xmp = true;
    return PC_OK;
}

static pc_status run_xmp(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    if (p != NULL && !yyjson_is_obj(p) && !yyjson_is_null(p)) {
        return pc_engine_err_bad_params(err, CMD, "expected an object");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }

    pc_layer_id id;
    yyjson_val *jl = p != NULL ? pc_json_get(p, "layer") : NULL;
    if (jl != NULL && !yyjson_is_null(jl)) {
        uint64_t idv = 0;
        if (!pc_json_as_u64(jl, &idv)) {
            return pc_engine_err_bad_params(err, CMD, "`layer` must be an unsigned integer");
        }
        id = (pc_layer_id)idv;
    } else {
        if (!d->has_active_layer) {
            return pc_engine_err_bad_params(err, CMD, "no active layer");
        }
        id = d->active_layer;
    }

    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    const char *current = l->has_xmp ? pc_str_c(&l->xmp) : NULL;
    size_t current_len = l->has_xmp ? l->xmp.len : 0;

    yyjson_val *jr = p != NULL ? pc_json_get(p, "rawData") : NULL;
    bool set = jr != NULL;
    bool remove = false;
    const char *newraw = NULL;
    size_t newlen = 0;
    if (set) {
        if (yyjson_is_null(jr)) {
            remove = true;
        } else if (yyjson_is_str(jr)) {
            newraw = yyjson_get_str(jr);
            newlen = yyjson_get_len(jr);
            pc_status rc = check_packet(newraw, newlen, err);
            if (rc != PC_OK) {
                return rc;
            }
        } else {
            return pc_engine_err_bad_params(err, CMD,
                                            "`rawData` must be a string (an XMP packet) or null (remove it)");
        }
    }

    /* Did it change? */
    bool changed;
    if (!set) {
        changed = false;
    } else if (remove) {
        changed = current != NULL;
    } else {
        changed = current == NULL || current_len != newlen || memcmp(current, newraw, newlen) != 0;
    }

    if (changed) {
        struct xmp_ctx c = {id, true, remove, newraw, newlen};
        pc_status rc = pc_engine_session_edit(s, "Layer Metadata", xmp_edit, &c, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_engine_doc_state *dm = pc_engine_session_active_mut(s);
        if (dm != NULL) { /* metadata only; no canvas pixels need recompositing */
            dm->has_last_damage = true;
            dm->last_damage = PC_RECT_EMPTY;
        }
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    /* Report the value as it now is: the new value when set, else the current one. */
    if (set) {
        if (remove) {
            yyjson_mut_obj_add_null(rd, root, "rawData");
        } else {
            yyjson_mut_obj_add_strncpy(rd, root, "rawData", newraw, newlen);
        }
    } else if (current != NULL) {
        yyjson_mut_obj_add_strncpy(rd, root, "rawData", current, current_len);
    } else {
        yyjson_mut_obj_add_null(rd, root, "rawData");
    }
    yyjson_mut_obj_add_bool(rd, root, "changed", changed);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

#define XMP_PARAMS                                                                                 \
    "{\"layer\":id?,\"rawData\":string|null?} -> {layer,rawData,changed} (no layer: the active "   \
    "layer; no rawData: read it; a string sets the layer's XMP packet, null removes it)"

static const pc_engine_command SPECS[] = {
    {CMD, "Layer XMP Metadata", NULL, 0, NULL, XMP_PARAMS, xmp_enabled, run_xmp, true},
};

const pc_engine_command *pc_engine_layer_xmp_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
