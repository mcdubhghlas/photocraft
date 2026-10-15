/* File > Print and File > Export > Paths to Illustrator (port of crates/engine/src/print_cmds.rs,
 * chunk E3).
 *
 * Paths to Illustrator writes the document's paths as an Adobe Illustrator 3 compatible PostScript
 * file (m/L/C path construction, n/N unpainted closed/open paths, compound-path markers). It is
 * self-contained and portable.
 *
 * DEFERRED (not registered here; see §7 of the report): File > Print, Print One Copy, Printers and
 * Package. Printing renders the flattened document into a one-page PDF (ISO 32000) and hands the
 * bytes to a system spooler (CUPS / Windows), neither of which has a C port yet (no craft_print,
 * and the ~300-line PDF writer is a follow-up); Package relinks linked smart objects, which needs
 * the still-unported layer.smartObjects.relinkToFile (Session smart_links + io linked). */
#include <math.h>
#include <stdio.h>

#include "base/buf.h"
#include "doc/doc.h"
#include "doc/vector.h"
#include "engine_internal.h"

/* The document's paths as an Illustrator 3 PostScript file. `which`: "all", "work" or a path name.
 * Coordinates are points (pixels at the document resolution), y up. *count gets the paths written.
 * Port of print_cmds::paths_to_ai. */
static pc_status pr_paths_to_ai(const pc_document *doc, const char *which, pc_buf *out, size_t *count,
                                pc_error *err)
{
    double dpi = (double)doc->resolution_dpi;
    if (dpi < 1.0) {
        dpi = 1.0;
    }
    double k = 72.0 / dpi;
    double w = (double)doc->size.width * k, h = (double)doc->size.height * k;
    pc_buf_init(out);
    pc_status rc = pc_buf_append_str(out, "%!PS-Adobe-2.0 EPSF-1.2\n%%Creator: PhotoCraft\n", err);
    if (rc != PC_OK) {
        return rc;
    }
    /* Title: the document name with parentheses replaced. */
    pc_buf title = PC_BUF_INIT;
    for (const char *c = pc_str_c(&doc->name); *c != '\0'; c++) {
        (void)pc_buf_append_byte(&title, (*c == '(' || *c == ')') ? (uint8_t)'_' : (uint8_t)*c, NULL);
    }
    rc = pc_buf_appendf(out, err, "%%%%Title: (%s)\n", pc_buf_cstr(&title));
    pc_buf_fini(&title);
    if (rc == PC_OK) {
        rc = pc_buf_appendf(
            out, err,
            "%%%%BoundingBox: 0 0 %lld %lld\n%%%%HiResBoundingBox: 0 0 %.4f %.4f\n"
            "%%AI3_Cropmarks: 0 0 %.4f %.4f\n",
            (long long)ceil(w), (long long)ceil(h), w, h, w, h);
    }
    if (rc == PC_OK) {
        rc = pc_buf_append_str(out,
                               "%%DocumentProcessColors: Black\n%%EndComments\n%%EndProlog\n"
                               "%%BeginSetup\n%%EndSetup\n",
                               err);
    }
    if (rc != PC_OK) {
        return rc;
    }

    /* The chosen paths: named paths (for "all" or a matching name) then the work path. */
    bool want_work = strcmp(which, "all") == 0 || strcmp(which, "work") == 0;
    size_t n = 0;
    /* Iterate named paths (for "all" or a matching name), then the work path (for "all"/"work"). */
    for (int pass = 0; pass < 2; pass++) {
        size_t npaths = pass == 0 ? doc->npaths : (doc->has_work_path ? 1 : 0);
        for (size_t pi = 0; pi < npaths; pi++) {
            const pc_path *path;
            const char *name;
            if (pass == 0) {
                const pc_named_path *np = &doc->paths[pi];
                name = pc_str_c(&np->name);
                if (strcmp(which, "all") != 0 && strcmp(which, "work") != 0 &&
                    strcmp(which, name) != 0) {
                    continue; /* by-name filter */
                }
                if (strcmp(which, "work") == 0) {
                    continue; /* work-only skips named */
                }
                path = &np->path;
            } else {
                if (!want_work) {
                    break;
                }
                name = "Work Path";
                path = &doc->work_path;
            }
            /* Non-empty subpaths. */
            size_t nonempty = 0;
            for (size_t si = 0; si < path->nsubpaths; si++) {
                if (path->subpaths[si].nknots > 0) {
                    nonempty++;
                }
            }
            if (nonempty == 0) {
                continue;
            }
            n++;
            rc = pc_buf_appendf(out, err, "%%%%Note: %s\n", name);
            if (rc != PC_OK) {
                return rc;
            }
            bool compound = nonempty > 1;
            if (compound) {
                rc = pc_buf_append_str(out, "*u\n", err);
                if (rc != PC_OK) {
                    return rc;
                }
            }
            for (size_t si = 0; si < path->nsubpaths; si++) {
                const pc_subpath *sp = &path->subpaths[si];
                if (sp->nknots == 0) {
                    continue;
                }
                double ax = sp->knots[0].anchor.x * k, ay = h - sp->knots[0].anchor.y * k;
                rc = pc_buf_appendf(out, err, "%.4f %.4f m\n", ax, ay);
                if (rc != PC_OK) {
                    return rc;
                }
                size_t segs = pc_subpath_segment_count(sp);
                for (size_t gi = 0; gi < segs; gi++) {
                    pc_point seg[4];
                    pc_subpath_segment(sp, gi, seg);
                    bool straight = seg[1].x == seg[0].x && seg[1].y == seg[0].y &&
                                    seg[2].x == seg[3].x && seg[2].y == seg[3].y;
                    double bx = seg[3].x * k, by = h - seg[3].y * k;
                    if (straight) {
                        rc = pc_buf_appendf(out, err, "%.4f %.4f L\n", bx, by);
                    } else {
                        double x1 = seg[1].x * k, y1 = h - seg[1].y * k;
                        double x2 = seg[2].x * k, y2 = h - seg[2].y * k;
                        rc = pc_buf_appendf(out, err, "%.4f %.4f %.4f %.4f %.4f %.4f C\n", x1, y1, x2,
                                            y2, bx, by);
                    }
                    if (rc != PC_OK) {
                        return rc;
                    }
                }
                rc = pc_buf_append_str(out, sp->closed ? "n\n" : "N\n", err);
                if (rc != PC_OK) {
                    return rc;
                }
            }
            if (compound) {
                rc = pc_buf_append_str(out, "*U\n", err);
                if (rc != PC_OK) {
                    return rc;
                }
            }
        }
    }
    rc = pc_buf_append_str(out, "%%PageTrailer\n%%Trailer\n%%EOF\n", err);
    if (rc != PC_OK) {
        return rc;
    }
    *count = n;
    return PC_OK;
}

static pc_status pr_has_paths(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        if (reason != NULL) {
            pc_fail_set(reason, PC_ERR_INVALID, "no document open");
        }
        return PC_ERR_INVALID;
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc->npaths == 0 && !doc->has_work_path) {
        if (reason != NULL) {
            pc_fail_set(reason, PC_ERR_INVALID, "the document has no paths");
        }
        return PC_ERR_INVALID;
    }
    return PC_OK;
}

static pc_status run_paths_to_illustrator(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                          pc_error *err)
{
    const char *cmd = "file.export.pathsToIllustrator";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const char *which = "all";
    yyjson_val *wv = pc_json_get(p, "paths");
    size_t wl = 0;
    const char *ws = wv != NULL ? pc_json_as_str(wv, &wl) : NULL;
    if (ws != NULL && wl > 0) {
        which = ws;
    }
    pc_buf text = PC_BUF_INIT;
    size_t n = 0;
    pc_status rc = pr_paths_to_ai(pc_doc_arc_get(d->doc), which, &text, &n, err);
    if (rc != PC_OK) {
        pc_buf_fini(&text);
        return rc;
    }
    if (n == 0) {
        pc_buf_fini(&text);
        return pc_engine_err_bad_params(err, cmd, "no paths match `%s`", which);
    }
    yyjson_val *pathv = pc_json_get(p, "path");
    size_t pl = 0;
    const char *path = pathv != NULL ? pc_json_as_str(pathv, &pl) : NULL;
    yyjson_mut_doc *jd = NULL;
    if (path != NULL && pl > 0) {
        rc = pc_engine_write_file(path, text.data, text.len, err);
        pc_buf_fini(&text);
        if (rc != PC_OK) {
            return rc;
        }
        if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(jd);
        yyjson_mut_doc_set_root(jd, root);
        yyjson_mut_obj_add_strcpy(jd, root, "path", path);
        yyjson_mut_obj_add_uint(jd, root, "paths", n);
        return PC_OK;
    }
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        pc_buf_fini(&text);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_doc_set_root(jd, root);
    yyjson_mut_obj_add_strcpy(jd, root, "ai", pc_buf_cstr(&text));
    pc_buf_fini(&text);
    yyjson_mut_obj_add_uint(jd, root, "paths", n);
    return PC_OK;
}

static const char *const MENU_FILE_EXPORT[] = {"File", "Export"};

const pc_engine_command *pc_engine_print_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"file.export.pathsToIllustrator", "Paths to Illustrator\xe2\x80\xa6", MENU_FILE_EXPORT, 2,
         NULL,
         "{\"path\":str? (.ai; omit to return the text),\"paths\":\"all|work|<path name>\"=\"all\"}"
         " -> {path, paths}",
         pr_has_paths, run_paths_to_illustrator, true},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
