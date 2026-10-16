/* File > Print, Print One Copy, Printers, Package and File > Export > Paths to Illustrator
 * (port of crates/engine/src/print_cmds.rs, chunk E3 / PRINT).
 *
 * Printing renders the flattened, colour-handled document into a one-page PDF (ISO 32000: paper
 * size, orientation, position, scale, colour handling with a printer profile / intent / black-point
 * compensation, corner and centre crop marks, registration marks, a label, Emulsion Down page
 * mirroring) with a small self-contained PDF writer (one page, Flate-compressed image XObject,
 * vector marks, Helvetica text), then hands the bytes to the system spooler.
 *
 * The Rust `craft_print` spooler has no C port; this file implements the POSIX path directly
 * (REWRITE_PLAN.md §7): printers are enumerated with `lpstat -p -d` / `lpstat -e` and jobs are
 * piped to `lp` through posix_spawn (never system()), the print-ready PDF streamed on stdin so no
 * shared temporary file can leak the document and no path appears in the process list. The generic
 * "Craft"/Photocraft-dev identity carries no personal data in any argument. Where no spooler is
 * present the commands still render the PDF (to an explicit `output` path) and return a clear
 * error. The Windows `craft_print` shim (PDF printing through powershell) is deferred (§6/§7).
 * `"dryRun": true` renders the PDF and reports the backend command without spooling.
 *
 * Paths to Illustrator writes the document's paths as an Adobe Illustrator 3 compatible PostScript
 * file (m/L/C path construction, n/N unpainted closed/open paths, compound-path markers). */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base/blob.h"
#include "base/buf.h"
#include "base/compress.h"
#include "base/mem.h"
#include "base/str.h"
#include "color/color.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "doc/vector.h"
#include "engine_internal.h"
#include "raster/surface.h"

extern char **environ;

/* NUL-terminated string duplicate (NULL-safe: NULL -> empty string). NULL on OOM. */
static char *pr_strdup(const char *s)
{
    if (s == NULL) {
        s = "";
    }
    return pc_strndup(s, strlen(s));
}
#define pc_strdup(s) pr_strdup(s)

/* ---- Printing disabled gate (print_cmds::disable_printing / printing_disabled) ------------- *
 * A test harness (the panic hunt) or CI sets this so no command in the process can reach a
 * printer; a dry run still renders and reports its command. */
static atomic_bool g_printing_disabled = false;

void pc_engine_print_disable(void)
{
    atomic_store_explicit(&g_printing_disabled, true, memory_order_relaxed);
}

static bool pr_printing_disabled(void)
{
    if (atomic_load_explicit(&g_printing_disabled, memory_order_relaxed)) {
        return true;
    }
    const char *v = getenv("PHOTOCRAFT_NO_PRINT");
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

/* ========================================================================================= *
 * Paths to Illustrator (unchanged; self-contained and portable).
 * ========================================================================================= */

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
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc->npaths == 0 && !doc->has_work_path) {
        return pc_fail(reason, PC_ERR_INVALID, "the document has no paths");
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

/* ========================================================================================= *
 * PDF writer (print_cmds::print_pdf and its helpers).
 * ========================================================================================= */

/* Paper sizes in points (portrait): name, width, height. */
static const struct {
    const char *name;
    double w, h;
} PR_PAPERS[] = {
    {"letter", 612.0, 792.0}, {"legal", 612.0, 1008.0}, {"tabloid", 792.0, 1224.0},
    {"a3", 841.89, 1190.55},  {"a4", 595.28, 841.89},   {"a5", 419.53, 595.28},
    {"4x6", 288.0, 432.0},    {"5x7", 360.0, 504.0},
};
#define PR_NPAPERS (sizeof PR_PAPERS / sizeof PR_PAPERS[0])
#define PR_PAPER_NAMES "letter|legal|tabloid|a3|a4|a5|4x6|5x7"

typedef struct {
    bool corner_crop;
    bool center_crop;
    bool registration;
} pr_marks;

/* An 8-bit interleaved print image: 1 (gray), 3 (RGB) or 4 (CMYK) colour channels. */
typedef struct {
    uint32_t width, height;
    size_t channels;
    uint8_t *data; /* width*height*channels, owned */
    size_t data_len;
    uint8_t *icc; /* owned, or NULL */
    size_t icc_len;
} pr_image;

static void pr_image_fini(pr_image *im)
{
    if (im == NULL) {
        return;
    }
    free(im->data);
    free(im->icc);
    im->data = NULL;
    im->icc = NULL;
}

/* A PDF (string) literal: ( ) and \ escaped, non-ASCII / control replaced with '?'. */
static pc_status pr_pdf_string(pc_buf *out, const char *s, pc_error *err)
{
    pc_status rc = pc_buf_append_byte(out, (uint8_t)'(', err);
    for (const unsigned char *c = (const unsigned char *)s; rc == PC_OK && *c != '\0'; c++) {
        if (*c == '(' || *c == ')' || *c == '\\') {
            rc = pc_buf_append_byte(out, (uint8_t)'\\', err);
            if (rc == PC_OK) {
                rc = pc_buf_append_byte(out, *c, err);
            }
        } else if (*c < 0x80 && *c >= 0x20) {
            rc = pc_buf_append_byte(out, *c, err);
        } else {
            rc = pc_buf_append_byte(out, (uint8_t)'?', err);
        }
    }
    if (rc == PC_OK) {
        rc = pc_buf_append_byte(out, (uint8_t)')', err);
    }
    return rc;
}

static pc_status pr_line(pc_buf *s, double x0, double y0, double x1, double y1, pc_error *err)
{
    return pc_buf_appendf(s, err, "%.2f %.2f m %.2f %.2f l S\n", x0, y0, x1, y1);
}

/* Vector marks around (x, y, w, h). Port of marks_ops. */
static pc_status pr_marks_ops(pc_buf *s, pr_marks m, double x, double y, double w, double h,
                              pc_error *err)
{
    pc_status rc = pc_buf_append_str(s, "q 0 G 0.3 w\n", err);
    const double gap = 6.0, len = 18.0;
    if (rc == PC_OK && m.corner_crop) {
        const double cs[4][4] = {{x, y, -1.0, -1.0},
                                 {x + w, y, 1.0, -1.0},
                                 {x, y + h, -1.0, 1.0},
                                 {x + w, y + h, 1.0, 1.0}};
        for (int i = 0; i < 4 && rc == PC_OK; i++) {
            double cx = cs[i][0], cy = cs[i][1], dx = cs[i][2], dy = cs[i][3];
            rc = pr_line(s, cx + dx * gap, cy, cx + dx * (gap + len), cy, err);
            if (rc == PC_OK) {
                rc = pr_line(s, cx, cy + dy * gap, cx, cy + dy * (gap + len), err);
            }
        }
    }
    if (rc == PC_OK && m.center_crop) {
        double mx = x + w / 2.0, my = y + h / 2.0;
        rc = pr_line(s, mx, y - gap, mx, y - gap - len, err);
        if (rc == PC_OK) {
            rc = pr_line(s, mx, y + h + gap, mx, y + h + gap + len, err);
        }
        if (rc == PC_OK) {
            rc = pr_line(s, x - gap, my, x - gap - len, my, err);
        }
        if (rc == PC_OK) {
            rc = pr_line(s, x + w + gap, my, x + w + gap + len, my, err);
        }
    }
    if (rc == PC_OK && m.registration) {
        const double r = 5.0, off = gap + len / 2.0, k = 0.5523 * r;
        const double ctr[4][2] = {{x + w / 2.0, y - off},
                                  {x + w / 2.0, y + h + off},
                                  {x - off, y + h / 2.0},
                                  {x + w + off, y + h / 2.0}};
        for (int i = 0; i < 4 && rc == PC_OK; i++) {
            double cx = ctr[i][0], cy = ctr[i][1];
            rc = pc_buf_appendf(
                s, err,
                "%.2f %.2f m %.2f %.2f %.2f %.2f %.2f %.2f c %.2f %.2f %.2f %.2f %.2f %.2f c "
                "%.2f %.2f %.2f %.2f %.2f %.2f c %.2f %.2f %.2f %.2f %.2f %.2f c S\n",
                cx + r, cy, cx + r, cy + k, cx + k, cy + r, cx, cy + r, cx - k, cy + r, cx - r,
                cy + k, cx - r, cy, cx - r, cy - k, cx - k, cy - r, cx, cy - r, cx + k, cy - r,
                cx + r, cy - k, cx + r, cy);
            if (rc == PC_OK) {
                rc = pr_line(s, cx - r * 1.6, cy, cx + r * 1.6, cy, err);
            }
            if (rc == PC_OK) {
                rc = pr_line(s, cx, cy - r * 1.6, cx, cy + r * 1.6, err);
            }
        }
    }
    if (rc == PC_OK) {
        rc = pc_buf_append_str(s, "Q\n", err);
    }
    return rc;
}

/* Deflate (zlib) at level 1 (print PDFs are transient and large); append to *out. */
static pc_status pr_deflate(const uint8_t *data, size_t len, pc_buf *out, pc_error *err)
{
    pc_buf_init(out);
    return pc_deflate(PC_Z_ZLIB, PC_Z_FAST, data, len, out, err);
}

/* Emit one object (records its byte offset). Port of print_pdf's `obj` closure. */
static pc_status pr_obj(pc_buf *out, size_t *offsets, size_t *nobj, const uint8_t *body, size_t blen,
                        pc_error *err)
{
    offsets[*nobj] = out->len;
    pc_status rc = pc_buf_appendf(out, err, "%zu 0 obj\n", *nobj + 1);
    if (rc == PC_OK) {
        rc = pc_buf_append(out, body, blen, err);
    }
    if (rc == PC_OK) {
        rc = pc_buf_append_str(out, "\nendobj\n", err);
    }
    if (rc == PC_OK) {
        (*nobj)++;
    }
    return rc;
}

/* A one-page print PDF. Port of print_cmds::print_pdf. */
static pc_status pr_print_pdf(double pw, double ph, const pr_image *img, double x, double y, double w,
                              double h, pr_marks marks, const char *label, const char *description,
                              bool emulsion_down, pc_buf *out, pc_error *err)
{
    static const uint8_t HEADER[] = {'%', 'P', 'D', 'F', '-', '1', '.', '4', '\n',
                                     '%', 0xe2, 0xe3, 0xcf, 0xd3, '\n'};
    pc_buf_init(out);
    pc_status rc = pc_buf_append(out, HEADER, sizeof HEADER, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t offsets[8] = {0};
    size_t nobj = 0;

    /* 1 catalog, 2 pages, 3 page, 4 contents, 5 image, 6 font, 7 ICC (optional). */
    rc = pr_obj(out, offsets, &nobj, (const uint8_t *)"<< /Type /Catalog /Pages 2 0 R >>", 33, err);
    if (rc == PC_OK) {
        rc = pr_obj(out, offsets, &nobj,
                    (const uint8_t *)"<< /Type /Pages /Kids [3 0 R] /Count 1 >>", 41, err);
    }
    if (rc == PC_OK) {
        pc_buf page = PC_BUF_INIT;
        rc = pc_buf_appendf(&page, err,
                            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << "
                            "/XObject << /Im0 5 0 R >> /Font << /F1 6 0 R >> >> /Contents 4 0 R >>",
                            pw, ph);
        if (rc == PC_OK) {
            rc = pr_obj(out, offsets, &nobj, page.data, page.len, err);
        }
        pc_buf_fini(&page);
    }
    if (rc != PC_OK) {
        return rc;
    }

    /* Content stream (object 4). */
    pc_buf content = PC_BUF_INIT;
    rc = pc_buf_appendf(&content, err, "q 1 g 0 0 %.2f %.2f re f Q\n", pw, ph);
    if (rc == PC_OK && emulsion_down) {
        rc = pc_buf_appendf(&content, err, "q -1 0 0 1 %.2f 0 cm\n", pw);
    }
    if (rc == PC_OK) {
        rc = pc_buf_appendf(&content, err, "q %.3f 0 0 %.3f %.3f %.3f cm /Im0 Do Q\n", w, h, x, y);
    }
    if (rc == PC_OK) {
        rc = pr_marks_ops(&content, marks, x, y, w, h, err);
    }
    bool any_marks = marks.corner_crop || marks.center_crop || marks.registration;
    double pad = any_marks ? 30.0 : 6.0;
    if (rc == PC_OK && label != NULL && label[0] != '\0') {
        rc = pc_buf_appendf(&content, err, "BT /F1 8 Tf 0 g %.2f %.2f Td ", x, y + h + pad);
        if (rc == PC_OK) {
            rc = pr_pdf_string(&content, label, err);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append_str(&content, " Tj ET\n", err);
        }
    }
    if (rc == PC_OK && description != NULL && description[0] != '\0') {
        rc = pc_buf_appendf(&content, err, "BT /F1 8 Tf 0 g %.2f %.2f Td ", x, y - pad - 8.0);
        if (rc == PC_OK) {
            rc = pr_pdf_string(&content, description, err);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append_str(&content, " Tj ET\n", err);
        }
    }
    if (rc == PC_OK && emulsion_down) {
        rc = pc_buf_append_str(&content, "Q\n", err);
    }
    if (rc == PC_OK) {
        pc_buf c = PC_BUF_INIT;
        rc = pr_deflate(content.data, content.len, &c, err);
        if (rc == PC_OK) {
            pc_buf body = PC_BUF_INIT;
            rc = pc_buf_appendf(&body, err, "<< /Length %zu /Filter /FlateDecode >>\nstream\n", c.len);
            if (rc == PC_OK) {
                rc = pc_buf_append(&body, c.data, c.len, err);
            }
            if (rc == PC_OK) {
                rc = pc_buf_append_str(&body, "\nendstream", err);
            }
            if (rc == PC_OK) {
                rc = pr_obj(out, offsets, &nobj, body.data, body.len, err);
            }
            pc_buf_fini(&body);
        }
        pc_buf_fini(&c);
    }
    pc_buf_fini(&content);
    if (rc != PC_OK) {
        return rc;
    }

    /* Image XObject (object 5). */
    const char *device = img->channels == 1 ? "/DeviceGray"
                         : img->channels == 4 ? "/DeviceCMYK"
                                              : "/DeviceRGB";
    pc_buf z = PC_BUF_INIT;
    rc = pr_deflate(img->data, img->data_len, &z, err);
    if (rc == PC_OK) {
        pc_buf body = PC_BUF_INIT;
        if (img->icc != NULL) {
            rc = pc_buf_appendf(&body, err,
                                "<< /Type /XObject /Subtype /Image /Width %u /Height %u /ColorSpace "
                                "[/ICCBased 7 0 R] /BitsPerComponent 8 /Filter /FlateDecode /Length "
                                "%zu >>\nstream\n",
                                img->width, img->height, z.len);
        } else {
            rc = pc_buf_appendf(&body, err,
                                "<< /Type /XObject /Subtype /Image /Width %u /Height %u /ColorSpace "
                                "%s /BitsPerComponent 8 /Filter /FlateDecode /Length %zu "
                                ">>\nstream\n",
                                img->width, img->height, device, z.len);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append(&body, z.data, z.len, err);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append_str(&body, "\nendstream", err);
        }
        if (rc == PC_OK) {
            rc = pr_obj(out, offsets, &nobj, body.data, body.len, err);
        }
        pc_buf_fini(&body);
    }
    pc_buf_fini(&z);
    if (rc != PC_OK) {
        return rc;
    }

    /* Font (object 6). */
    rc = pr_obj(out, offsets, &nobj,
                (const uint8_t *)"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                                 "/WinAnsiEncoding >>",
                80, err);
    if (rc != PC_OK) {
        return rc;
    }

    /* ICC profile (object 7, optional). */
    if (img->icc != NULL) {
        pc_buf zicc = PC_BUF_INIT;
        rc = pr_deflate(img->icc, img->icc_len, &zicc, err);
        if (rc == PC_OK) {
            pc_buf body = PC_BUF_INIT;
            rc = pc_buf_appendf(&body, err,
                                "<< /N %zu /Alternate %s /Filter /FlateDecode /Length %zu "
                                ">>\nstream\n",
                                img->channels, device, zicc.len);
            if (rc == PC_OK) {
                rc = pc_buf_append(&body, zicc.data, zicc.len, err);
            }
            if (rc == PC_OK) {
                rc = pc_buf_append_str(&body, "\nendstream", err);
            }
            if (rc == PC_OK) {
                rc = pr_obj(out, offsets, &nobj, body.data, body.len, err);
            }
            pc_buf_fini(&body);
        }
        pc_buf_fini(&zicc);
        if (rc != PC_OK) {
            return rc;
        }
    }

    /* Cross-reference table and trailer. */
    size_t xref = out->len;
    rc = pc_buf_appendf(out, err, "xref\n0 %zu\n0000000000 65535 f \n", nobj + 1);
    for (size_t i = 0; rc == PC_OK && i < nobj; i++) {
        rc = pc_buf_appendf(out, err, "%010zu 00000 n \n", offsets[i]);
    }
    if (rc == PC_OK) {
        rc = pc_buf_appendf(out, err, "trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
                            nobj + 1, xref);
    }
    return rc;
}

/* ========================================================================================= *
 * Colour-handled flattening (print_cmds::print_image).
 * ========================================================================================= */

/* Colour-handling diagnostics for the "color" result field. */
typedef struct {
    char handling[24];
    bool has_profile;
    char *profile; /* owned, or NULL */
    char intent[16];
    bool has_intent;
    bool bpc;
    bool has_bpc;
} pr_color_info;

static void pr_color_fini(pr_color_info *ci)
{
    if (ci != NULL) {
        free(ci->profile);
        ci->profile = NULL;
    }
}

static uint8_t pr_to8(float v)
{
    if (!(v > 0.0f)) {
        v = 0.0f; /* also catches NaN */
    }
    if (v > 1.0f) {
        v = 1.0f;
    }
    return (uint8_t)lrintf(v * 255.0f);
}

/* Execute a command on a scratch session, discarding the result. */
static pc_status pr_exec(pc_engine_session *t, const char *id, yyjson_mut_doc *params, pc_error *err)
{
    yyjson_doc *imut = NULL;
    yyjson_val *root = NULL;
    if (params != NULL) {
        imut = yyjson_mut_doc_imut_copy(params, NULL);
        yyjson_mut_doc_free(params);
        if (imut == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
        }
        root = yyjson_doc_get_root(imut);
    }
    pc_json_value res = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute(t, id, root, &res, err);
    pc_json_value_fini(&res);
    if (imut != NULL) {
        yyjson_doc_free(imut);
    }
    return rc;
}

/* The flattened, colour-handled document as 8-bit samples in its print colour space. Port of
 * print_cmds::print_image. */
static pc_status pr_print_image(const pc_document *doc, yyjson_val *p, const char *cmd, pr_image *out,
                                pr_color_info *color, pc_error *err)
{
    memset(out, 0, sizeof *out);
    memset(color, 0, sizeof *color);

    const char *handling = "printerManages";
    yyjson_val *hv = pc_json_get(p, "colorHandling");
    size_t hl = 0;
    const char *hs = hv != NULL ? pc_json_as_str(hv, &hl) : NULL;
    if (hs != NULL) {
        handling = hs;
    }
    snprintf(color->handling, sizeof color->handling, "%s", handling);

    pc_engine_session *t = NULL;
    pc_status rc = pc_engine_session_new(&t, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_document clone;
    rc = pc_document_clone(doc, &clone, err);
    if (rc != PC_OK) {
        pc_engine_session_free(t);
        return rc;
    }
    size_t idx = 0;
    rc = pc_engine_session_add_document(t, &clone, NULL, &idx, err);
    if (rc != PC_OK) {
        pc_document_fini(&clone);
        pc_engine_session_free(t);
        return rc;
    }

    const pc_engine_doc_state *ds = pc_engine_session_active(t);
    pc_color_mode mode0 = ds != NULL ? pc_doc_arc_get(ds->doc)->mode : PC_COLOR_MODE_RGB;
    if (mode0 != PC_COLOR_MODE_RGB && mode0 != PC_COLOR_MODE_GRAYSCALE && mode0 != PC_COLOR_MODE_CMYK) {
        rc = pr_exec(t, "image.mode.rgb", NULL, err);
        if (rc != PC_OK) {
            pc_engine_session_free(t);
            return rc;
        }
    }

    if (strcmp(handling, "photocraftManages") == 0 || strcmp(handling, "photoshopManages") == 0) {
        yyjson_val *pv = pc_json_get(p, "printerProfile");
        size_t pvl = 0;
        const char *profile = pv != NULL ? pc_json_as_str(pv, &pvl) : NULL;
        if (profile == NULL) {
            pc_engine_session_free(t);
            return pc_engine_err_bad_params(err, cmd, "\"photocraftManages\" needs a \"printerProfile\"");
        }
        const char *intent = "relative";
        yyjson_val *iv = pc_json_get(p, "intent");
        size_t ivl = 0;
        const char *is = iv != NULL ? pc_json_as_str(iv, &ivl) : NULL;
        if (is != NULL) {
            intent = is;
        }
        bool bpc = true;
        yyjson_val *bv = pc_json_get(p, "bpc");
        if (bv != NULL) {
            (void)pc_json_as_bool(bv, &bpc);
        }
        yyjson_mut_doc *cp = yyjson_mut_doc_new(NULL);
        if (cp == NULL) {
            pc_engine_session_free(t);
            return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
        }
        yyjson_mut_val *root = yyjson_mut_obj(cp);
        yyjson_mut_doc_set_root(cp, root);
        yyjson_mut_obj_add_strncpy(cp, root, "profile", profile, pvl);
        yyjson_mut_obj_add_strcpy(cp, root, "intent", intent);
        yyjson_mut_obj_add_bool(cp, root, "bpc", bpc);
        rc = pr_exec(t, "edit.convertToProfile", cp, err);
        if (rc != PC_OK) {
            pc_engine_session_free(t);
            return rc;
        }
        color->has_profile = true;
        color->profile = pc_strndup(profile, pvl);
        snprintf(color->intent, sizeof color->intent, "%s", intent);
        color->has_intent = true;
        color->bpc = bpc;
        color->has_bpc = true;
    } else if (strcmp(handling, "printerManages") == 0 || strcmp(handling, "noColorManagement") == 0) {
        /* nothing */
    } else if (strcmp(handling, "separations") == 0) {
        pc_engine_session_free(t);
        return pc_engine_err_bad_params(
            err, cmd,
            "Separations printing is not supported; use photocraftManages with a CMYK printer profile");
    } else {
        pc_engine_session_free(t);
        return pc_engine_err_bad_params(
            err, cmd, "unknown colorHandling `%s` (printerManages|photocraftManages|noColorManagement)",
            handling);
    }

    ds = pc_engine_session_active(t);
    if (ds == NULL) {
        pc_engine_session_free(t);
        return pc_engine_err_no_document(err);
    }
    const pc_document *d = pc_doc_arc_get(ds->doc);

    /* ICC profile unless colour management is turned off. */
    if (strcmp(handling, "noColorManagement") != 0 && d->icc_profile != NULL) {
        const uint8_t *ib = pc_blob_data(d->icc_profile);
        size_t il = pc_blob_len(d->icc_profile);
        if (ib != NULL && il > 0) {
            out->icc = pc_malloc_array(il, 1);
            if (out->icc == NULL) {
                pc_engine_session_free(t);
                return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
            }
            memcpy(out->icc, ib, il);
            out->icc_len = il;
        }
    }

    if (d->mode == PC_COLOR_MODE_RGB || d->mode == PC_COLOR_MODE_GRAYSCALE) {
        bool gray = d->mode == PC_COLOR_MODE_GRAYSCALE;
        pc_compose_buffer buf = {0};
        rc = pc_compose_flatten(d, &buf, err);
        if (rc != PC_OK) {
            pc_compose_buffer_fini(&buf);
            pc_engine_session_free(t);
            return rc;
        }
        const float white[3] = {1.0f, 1.0f, 1.0f};
        pc_compose_buffer_over_background(&buf, white);
        size_t ch = gray ? 1u : 3u;
        out->width = d->size.width;
        out->height = d->size.height;
        out->channels = ch;
        out->data_len = buf.len * ch;
        out->data = buf.len > 0 ? pc_malloc_array(buf.len, ch) : NULL;
        if (out->data == NULL && buf.len > 0) {
            pc_compose_buffer_fini(&buf);
            pc_engine_session_free(t);
            return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
        }
        for (size_t i = 0; i < buf.len; i++) {
            if (gray) {
                out->data[i] = pr_to8(buf.px[i][0]);
            } else {
                out->data[i * 3 + 0] = pr_to8(buf.px[i][0]);
                out->data[i * 3 + 1] = pr_to8(buf.px[i][1]);
                out->data[i * 3 + 2] = pr_to8(buf.px[i][2]);
            }
        }
        pc_compose_buffer_fini(&buf);
        pc_engine_session_free(t);
        return PC_OK;
    }

    /* CMYK (or any non-RGB/Gray mode after mode.rgb): flatten to one layer and read it. */
    rc = pr_exec(t, "layer.flattenImage", NULL, err);
    if (rc != PC_OK) {
        pc_engine_session_free(t);
        return rc;
    }
    ds = pc_engine_session_active(t);
    if (ds == NULL) {
        pc_engine_session_free(t);
        return pc_engine_err_no_document(err);
    }
    d = pc_doc_arc_get(ds->doc);
    pc_pixel_format fmt = pc_document_pixel_format(d);
    size_t k = pc_color_mode_channels(fmt.mode);
    size_t n = pc_pixel_format_channels(fmt);
    const pc_surface *surf = d->nlayers > 0 ? pc_layer_surface(&d->layers[0]) : NULL;
    if (surf == NULL || k == 0 || n == 0) {
        pc_engine_session_free(t);
        return pc_engine_err_other(err, "nothing to print");
    }
    pc_rect bounds = pc_document_bounds(d);
    float *vals = NULL;
    size_t vlen = 0;
    rc = pc_surface_read_region(surf, bounds, &vals, &vlen, err);
    if (rc != PC_OK) {
        free(vals);
        pc_engine_session_free(t);
        return rc;
    }
    size_t npx = vlen / n;
    out->width = d->size.width;
    out->height = d->size.height;
    out->channels = k;
    out->data_len = npx * k;
    out->data = npx > 0 ? pc_malloc_array(npx, k) : NULL;
    if (out->data == NULL && npx > 0) {
        free(vals);
        pc_engine_session_free(t);
        return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
    }
    for (size_t i = 0; i < npx; i++) {
        for (size_t c = 0; c < k; c++) {
            out->data[i * k + c] = pr_to8(vals[i * n + c]);
        }
    }
    free(vals);
    pc_engine_session_free(t);
    return PC_OK;
}

/* ========================================================================================= *
 * Layout (print_cmds::paper / layout).
 * ========================================================================================= */

typedef struct {
    double pw, ph;
    double x, y, w, h;
    double scale;
    pr_marks marks;
    bool emulsion_down;
} pr_layout;

static pc_status pr_paper(yyjson_val *p, const char *cmd, double *pw, double *ph, pc_error *err)
{
    double w, h;
    yyjson_val *pv = pc_json_get(p, "paper");
    if (pv == NULL) {
        w = 612.0;
        h = 792.0;
    } else if (yyjson_is_str(pv)) {
        size_t nl = 0;
        const char *name = pc_json_as_str(pv, &nl);
        size_t found = PR_NPAPERS;
        for (size_t i = 0; i < PR_NPAPERS; i++) {
            if (name != NULL && strcasecmp(name, PR_PAPERS[i].name) == 0) {
                found = i;
                break;
            }
        }
        if (found == PR_NPAPERS) {
            return pc_engine_err_bad_params(err, cmd, "unknown paper `%s` (%s)",
                                            name != NULL ? name : "", PR_PAPER_NAMES);
        }
        w = PR_PAPERS[found].w;
        h = PR_PAPERS[found].h;
    } else if (yyjson_is_arr(pv) && yyjson_arr_size(pv) == 2) {
        double a0 = 0.0, a1 = 0.0;
        (void)pc_json_as_f64(yyjson_arr_get(pv, 0), &a0);
        (void)pc_json_as_f64(yyjson_arr_get(pv, 1), &a1);
        w = a0;
        h = a1;
    } else {
        return pc_engine_err_bad_params(err, cmd, "paper is a name or [width, height] in points");
    }
    if (!(w > 36.0 && h > 36.0)) {
        return pc_engine_err_bad_params(err, cmd, "paper is too small");
    }
    yyjson_val *ov = pc_json_get(p, "orientation");
    size_t ol = 0;
    const char *os = ov != NULL ? pc_json_as_str(ov, &ol) : NULL;
    if (os != NULL && strcmp(os, "landscape") == 0) {
        *pw = h > w ? h : w;
        *ph = w < h ? w : h;
    } else {
        *pw = w;
        *ph = h;
    }
    return PC_OK;
}

static bool pr_bool(yyjson_val *p, const char *key, bool def)
{
    yyjson_val *v = pc_json_get(p, key);
    bool out = def;
    if (v != NULL) {
        (void)pc_json_as_bool(v, &out);
    }
    return out;
}

/* The print layout for `doc` with Print parameters `p`. Port of print_cmds::layout. */
static pc_status pr_compute_layout(const pc_document *doc, yyjson_val *p, const char *cmd, pr_layout *out,
                           pc_error *err)
{
    double pw = 0.0, ph = 0.0;
    pc_status rc = pr_paper(p, cmd, &pw, &ph, err);
    if (rc != PC_OK) {
        return rc;
    }
    double dpi = (double)doc->resolution_dpi;
    if (dpi < 1.0) {
        dpi = 1.0;
    }
    double iw = (double)doc->size.width * 72.0 / dpi;
    double ih = (double)doc->size.height * 72.0 / dpi;
    pr_marks marks = {pr_bool(p, "cornerCropMarks", false), pr_bool(p, "centerCropMarks", false),
                      pr_bool(p, "registrationMarks", false)};
    double margin = (marks.corner_crop || marks.center_crop || marks.registration) ? 36.0 : 18.0;
    bool fit = pr_bool(p, "scaleToFit", false);
    double scale;
    if (fit) {
        double sx = (pw - 2.0 * margin) / iw;
        double sy = (ph - 2.0 * margin) / ih;
        scale = sx < sy ? sx : sy;
    } else {
        double pct = 100.0;
        yyjson_val *sv = pc_json_get(p, "scale");
        double got;
        if (sv != NULL && pc_json_as_f64(sv, &got) && got > 0.0) {
            pct = got;
        }
        scale = pct / 100.0;
    }
    double w = iw * scale, h = ih * scale;
    bool center = pr_bool(p, "center", true);
    double x, y;
    if (center) {
        x = (pw - w) / 2.0;
        y = (ph - h) / 2.0;
    } else {
        double top = 0.0, left = 0.0;
        yyjson_val *tv = pc_json_get(p, "top");
        yyjson_val *lv = pc_json_get(p, "left");
        if (tv != NULL) {
            (void)pc_json_as_f64(tv, &top);
        }
        if (lv != NULL) {
            (void)pc_json_as_f64(lv, &left);
        }
        top *= 72.0;
        left *= 72.0;
        x = left;
        y = ph - top - h;
    }
    out->pw = pw;
    out->ph = ph;
    out->x = x;
    out->y = y;
    out->w = w;
    out->h = h;
    out->scale = scale;
    out->marks = marks;
    out->emulsion_down = pr_bool(p, "emulsionDown", false);
    return PC_OK;
}

/* ========================================================================================= *
 * POSIX spooler (replaces craft_print; lpstat enumeration + lp submission via posix_spawn).
 * ========================================================================================= */

/* Build the `lp` argv (NUL-terminated, [0] = "lp"). `printer` may be NULL/empty for the default
 * destination. Port of craft_print::lp_args, Job default (collate=true, Scaling::Exact). */
static char **pr_lp_argv(const char *printer, int copies, const char *title, size_t *out_n)
{
    char *items[16];
    size_t n = 0;
    items[n++] = pc_strdup("lp");
    if (printer != NULL && printer[0] != '\0') {
        items[n++] = pc_strdup("-d");
        items[n++] = pc_strdup(printer);
    }
    if (copies < 1) {
        copies = 1;
    }
    if (copies > 999) {
        copies = 999;
    }
    char cbuf[16];
    (void)snprintf(cbuf, sizeof cbuf, "%d", copies);
    items[n++] = pc_strdup("-n");
    items[n++] = pc_strdup(cbuf);
    items[n++] = pc_strdup("-t");
    items[n++] = pc_strdup(title != NULL ? title : "");
    items[n++] = pc_strdup("-o");
    items[n++] = pc_strdup("collate=true");
    items[n++] = pc_strdup("-o");
    items[n++] = pc_strdup("fit-to-page=false");
    char **argv = pc_malloc_array(n + 1, sizeof *argv);
    if (argv == NULL) {
        for (size_t i = 0; i < n; i++) {
            free(items[i]);
        }
        return NULL;
    }
    bool bad = false;
    for (size_t i = 0; i < n; i++) {
        argv[i] = items[i];
        if (items[i] == NULL) {
            bad = true;
        }
    }
    argv[n] = NULL;
    if (bad) {
        for (size_t i = 0; i < n; i++) {
            free(argv[i]);
        }
        free(argv);
        return NULL;
    }
    *out_n = n;
    return argv;
}

static void pr_free_argv(char **argv)
{
    if (argv == NULL) {
        return;
    }
    for (size_t i = 0; argv[i] != NULL; i++) {
        free(argv[i]);
    }
    free(argv);
}

#define PR_CAPTURE_MAX ((size_t)4 << 20) /* cap captured child output */

/* Run argv (with `envp` or `environ`), optionally piping `in` to its stdin, capturing stdout into
 * `out` and stderr into `errbuf` (either may be NULL to discard). *spawned is false when the
 * program could not be started (e.g. no spooler installed); *exitcode gets the exit status
 * (-1 if it did not exit normally). Never crashes; SIGPIPE is blocked for the duration. */
static pc_status pr_spawn(char *const argv[], char *const envp[], const uint8_t *in, size_t in_len,
                          pc_buf *out, pc_buf *errbuf, bool *spawned, int *exitcode, pc_error *err)
{
    (void)err; /* a failure to start is reported via *spawned, not as an error */
    *spawned = false;
    *exitcode = -1;
    int inpipe[2] = {-1, -1}, outpipe[2] = {-1, -1}, errpipe[2] = {-1, -1};
    pc_status st = PC_OK;
    if ((in != NULL && pipe(inpipe) != 0) || pipe(outpipe) != 0 || pipe(errpipe) != 0) {
        goto cleanup_pipes;
    }
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        goto cleanup_pipes;
    }
    int ok = 0;
    if (in != NULL) {
        ok |= posix_spawn_file_actions_adddup2(&fa, inpipe[0], 0);
        ok |= posix_spawn_file_actions_addclose(&fa, inpipe[0]);
        ok |= posix_spawn_file_actions_addclose(&fa, inpipe[1]);
    } else {
        ok |= posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    }
    ok |= posix_spawn_file_actions_adddup2(&fa, outpipe[1], 1);
    ok |= posix_spawn_file_actions_adddup2(&fa, errpipe[1], 2);
    ok |= posix_spawn_file_actions_addclose(&fa, outpipe[0]);
    ok |= posix_spawn_file_actions_addclose(&fa, outpipe[1]);
    ok |= posix_spawn_file_actions_addclose(&fa, errpipe[0]);
    ok |= posix_spawn_file_actions_addclose(&fa, errpipe[1]);
    pid_t pid = -1;
    if (ok != 0 ||
        posix_spawnp(&pid, argv[0], &fa, NULL, argv, envp != NULL ? envp : environ) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        goto cleanup_pipes; /* not installed: *spawned stays false, PC_OK */
    }
    posix_spawn_file_actions_destroy(&fa);
    *spawned = true;

    /* Parent keeps the writing end of stdin and the reading ends of stdout/stderr. */
    if (in != NULL) {
        close(inpipe[0]);
        inpipe[0] = -1;
    }
    close(outpipe[1]);
    outpipe[1] = -1;
    close(errpipe[1]);
    errpipe[1] = -1;

    /* Block SIGPIPE on this thread so a spooler that refuses the job before reading stdin cannot
     * terminate the process; we see EPIPE instead. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    bool masked = pthread_sigmask(SIG_BLOCK, &block, &old) == 0;

    if (in != NULL) {
        (void)fcntl(inpipe[1], F_SETFL, O_NONBLOCK);
    }
    (void)fcntl(outpipe[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(errpipe[0], F_SETFL, O_NONBLOCK);

    size_t in_pos = 0;
    bool out_open = true, err_open = true;
    while (out_open || err_open || (in != NULL && inpipe[1] >= 0)) {
        struct pollfd pf[3];
        int nf = 0;
        int ii = -1, io = -1, ie = -1;
        if (in != NULL && inpipe[1] >= 0) {
            pf[nf].fd = inpipe[1];
            pf[nf].events = POLLOUT;
            pf[nf].revents = 0;
            ii = nf++;
        }
        if (out_open) {
            pf[nf].fd = outpipe[0];
            pf[nf].events = POLLIN;
            pf[nf].revents = 0;
            io = nf++;
        }
        if (err_open) {
            pf[nf].fd = errpipe[0];
            pf[nf].events = POLLIN;
            pf[nf].revents = 0;
            ie = nf++;
        }
        if (nf == 0) {
            break;
        }
        int pr = poll(pf, (nfds_t)nf, -1);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ii >= 0 && (pf[ii].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
            size_t want = in_len - in_pos;
            if (want > 65536) {
                want = 65536;
            }
            ssize_t wr = want > 0 ? write(inpipe[1], in + in_pos, want) : 0;
            if (wr > 0) {
                in_pos += (size_t)wr;
            } else if (wr < 0 && (errno == EAGAIN || errno == EINTR)) {
                /* retry */
            } else {
                close(inpipe[1]); /* EPIPE or done */
                inpipe[1] = -1;
            }
            if (inpipe[1] >= 0 && in_pos >= in_len) {
                close(inpipe[1]);
                inpipe[1] = -1;
            }
        }
        if (io >= 0 && (pf[io].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            uint8_t chunk[8192];
            ssize_t got = read(outpipe[0], chunk, sizeof chunk);
            if (got > 0) {
                if (out != NULL && out->len < PR_CAPTURE_MAX) {
                    (void)pc_buf_append(out, chunk, (size_t)got, NULL);
                }
            } else if (got == 0) {
                close(outpipe[0]);
                outpipe[0] = -1;
                out_open = false;
            } else if (errno != EAGAIN && errno != EINTR) {
                close(outpipe[0]);
                outpipe[0] = -1;
                out_open = false;
            }
        }
        if (ie >= 0 && (pf[ie].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            uint8_t chunk[8192];
            ssize_t got = read(errpipe[0], chunk, sizeof chunk);
            if (got > 0) {
                if (errbuf != NULL && errbuf->len < PR_CAPTURE_MAX) {
                    (void)pc_buf_append(errbuf, chunk, (size_t)got, NULL);
                }
            } else if (got == 0) {
                close(errpipe[0]);
                errpipe[0] = -1;
                err_open = false;
            } else if (errno != EAGAIN && errno != EINTR) {
                close(errpipe[0]);
                errpipe[0] = -1;
                err_open = false;
            }
        }
    }
    if (in != NULL && inpipe[1] >= 0) {
        close(inpipe[1]);
        inpipe[1] = -1;
    }
    if (outpipe[0] >= 0) {
        close(outpipe[0]);
        outpipe[0] = -1;
    }
    if (errpipe[0] >= 0) {
        close(errpipe[0]);
        errpipe[0] = -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    *exitcode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    /* Drain any pending SIGPIPE, then restore the mask. */
    if (masked) {
        struct timespec zero = {0, 0};
        sigset_t pend;
        sigemptyset(&pend);
        sigaddset(&pend, SIGPIPE);
        (void)sigtimedwait(&pend, NULL, &zero);
        (void)pthread_sigmask(SIG_SETMASK, &old, NULL);
    }
    (void)st;
    return PC_OK;

cleanup_pipes:
    for (int i = 0; i < 2; i++) {
        if (inpipe[i] >= 0) {
            close(inpipe[i]);
        }
        if (outpipe[i] >= 0) {
            close(outpipe[i]);
        }
        if (errpipe[i] >= 0) {
            close(errpipe[i]);
        }
    }
    return PC_OK; /* could not run: *spawned false */
}

/* Trim ASCII whitespace from both ends in place; returns the start. */
static char *pr_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

/* craft_print::job_message: lp's "request id is …" line, without the "(0 file(s))" CUPS adds for a
 * stdin job. *out is an owned string. */
static char *pr_job_message(const char *stdout_text)
{
    char *dup = pc_strdup(stdout_text != NULL ? stdout_text : "");
    if (dup == NULL) {
        return NULL;
    }
    char *s = pr_trim(dup);
    char *tail = strstr(s, "(0 file(s))");
    if (tail != NULL) {
        *tail = '\0';
        /* trim trailing space the suffix left behind */
        size_t n = strlen(s);
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) {
            s[--n] = '\0';
        }
    }
    char *res = pc_strdup(s);
    free(dup);
    return res;
}

/* Submit a print-ready PDF to `lp`. On success *out_msg is an owned spooler message; on failure an
 * error. Port of craft_print::submit (CUPS). */
static pc_status pr_submit(const uint8_t *pdf, size_t pdf_len, const char *printer, int copies,
                           const char *title, char **out_msg, pc_error *err)
{
    *out_msg = NULL;
    size_t argn = 0;
    char **argv = pr_lp_argv(printer, copies, title, &argn);
    if (argv == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
    }
    pc_buf so = PC_BUF_INIT, se = PC_BUF_INIT;
    bool spawned = false;
    int code = -1;
    pc_status rc = pr_spawn(argv, NULL, pdf, pdf_len, &so, &se, &spawned, &code, err);
    pr_free_argv(argv);
    if (rc != PC_OK) {
        pc_buf_fini(&so);
        pc_buf_fini(&se);
        return rc;
    }
    if (!spawned) {
        pc_buf_fini(&so);
        pc_buf_fini(&se);
        return pc_engine_err_other(err, "the print spooler is not available: lp could not be started");
    }
    if (code != 0) {
        char *estr = pc_strdup(pc_buf_cstr(&se));
        char *trimmed = estr != NULL ? pr_trim(estr) : NULL;
        pc_status e = pc_engine_err_other(
            err, "%s", (trimmed != NULL && trimmed[0] != '\0') ? trimmed : "the print job was refused");
        free(estr);
        pc_buf_fini(&so);
        pc_buf_fini(&se);
        return e;
    }
    *out_msg = pr_job_message(pc_buf_cstr(&so));
    pc_buf_fini(&so);
    pc_buf_fini(&se);
    if (*out_msg == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
    }
    return PC_OK;
}

/* ---- printer enumeration (lpstat) ---- */

typedef struct {
    char *name;
    bool def;
} pr_printer;

/* Build an environ-derived envp with LC_ALL=C, LANG=C and SOFTWARE=Craft set, so lpstat prints
 * untranslated messages the parser reads. Returns a NULL-terminated array (caller frees each entry
 * then the array), or NULL on OOM. */
static char **pr_lpstat_env(void)
{
    static const char *const OVER[3] = {"LC_ALL=C", "LANG=C", "SOFTWARE=Craft"};
    static const char *const KEYS[3] = {"LC_ALL=", "LANG=", "SOFTWARE="};
    size_t base = 0;
    for (char **e = environ; e != NULL && *e != NULL; e++) {
        base++;
    }
    char **out = pc_malloc_array(base + 3 + 1, sizeof *out);
    if (out == NULL) {
        return NULL;
    }
    size_t n = 0;
    for (char **e = environ; e != NULL && *e != NULL; e++) {
        bool skip = false;
        for (int k = 0; k < 3; k++) {
            if (strncmp(*e, KEYS[k], strlen(KEYS[k])) == 0) {
                skip = true;
                break;
            }
        }
        if (!skip) {
            out[n] = pc_strdup(*e);
            if (out[n] == NULL) {
                goto oom;
            }
            n++;
        }
    }
    for (int k = 0; k < 3; k++) {
        out[n] = pc_strdup(OVER[k]);
        if (out[n] == NULL) {
            goto oom;
        }
        n++;
    }
    out[n] = NULL;
    return out;
oom:
    for (size_t i = 0; i < n; i++) {
        free(out[i]);
    }
    free(out);
    return NULL;
}

/* The `system default destination:` name from lpstat output (trimmed), into a static-free owned
 * string, or NULL. */
static char *pr_default_destination(const char *out)
{
    const char *key = "system default destination:";
    const char *line = out;
    while (line != NULL && *line != '\0') {
        const char *nl = strchr(line, '\n');
        size_t ll = nl != NULL ? (size_t)(nl - line) : strlen(line);
        if (ll >= strlen(key) && strncmp(line, key, strlen(key)) == 0) {
            char *dup = pc_strndup(line + strlen(key), ll - strlen(key));
            if (dup == NULL) {
                return NULL;
            }
            char *t = pr_trim(dup);
            char *res = pc_strdup(t);
            free(dup);
            return res;
        }
        line = nl != NULL ? nl + 1 : NULL;
    }
    return NULL;
}

/* Append printers from `lpstat -p -d` output (parse_lpstat). */
static pc_status pr_parse_lpstat(const char *out, pr_printer **list, size_t *n, size_t *cap,
                                 pc_error *err)
{
    char *def = pr_default_destination(out);
    const char *line = out;
    pc_status rc = PC_OK;
    while (line != NULL && *line != '\0' && rc == PC_OK) {
        const char *nl = strchr(line, '\n');
        size_t ll = nl != NULL ? (size_t)(nl - line) : strlen(line);
        if (ll > 8 && strncmp(line, "printer ", 8) == 0) {
            const char *name = line + 8;
            size_t namelen = 0;
            while (name + namelen < line + ll && name[namelen] != ' ' && name[namelen] != '\t') {
                namelen++;
            }
            if (namelen > 0) {
                pr_printer *grown = pc_grow_array(*list, cap, *n + 1, sizeof **list);
                if (grown == NULL) {
                    rc = pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
                    break;
                }
                *list = grown;
                char *nm = pc_strndup(name, namelen);
                if (nm == NULL) {
                    rc = pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
                    break;
                }
                (*list)[*n].name = nm;
                (*list)[*n].def = def != NULL && strcmp(def, nm) == 0;
                (*n)++;
            }
        }
        line = nl != NULL ? nl + 1 : NULL;
    }
    free(def);
    return rc;
}

/* Append driverless destinations from `lpstat -e` (parse_lpstat_e), skipping names already present;
 * `def` marks the default (from -d). */
static pc_status pr_parse_lpstat_e(const char *out, const char *def, pr_printer **list, size_t *n,
                                   size_t *cap, pc_error *err)
{
    const char *line = out;
    pc_status rc = PC_OK;
    while (line != NULL && *line != '\0' && rc == PC_OK) {
        const char *nl = strchr(line, '\n');
        size_t ll = nl != NULL ? (size_t)(nl - line) : strlen(line);
        char *dup = pc_strndup(line, ll);
        if (dup == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
            break;
        }
        char *name = pr_trim(dup);
        if (name[0] != '\0') {
            bool seen = false;
            for (size_t i = 0; i < *n; i++) {
                if (strcmp((*list)[i].name, name) == 0) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                pr_printer *grown = pc_grow_array(*list, cap, *n + 1, sizeof **list);
                if (grown == NULL) {
                    free(dup);
                    rc = pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
                    break;
                }
                *list = grown;
                char *nm = pc_strdup(name);
                if (nm == NULL) {
                    free(dup);
                    rc = pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
                    break;
                }
                (*list)[*n].name = nm;
                (*list)[*n].def = def != NULL && strcmp(def, name) == 0;
                (*n)++;
            }
        }
        free(dup);
        line = nl != NULL ? nl + 1 : NULL;
    }
    return rc;
}

/* The printers the system spooler knows (craft_print::printers). *list is an owned array (free each
 * name then the array); empty on no spooler. Never crashes. */
static pc_status pr_printers(pr_printer **list, size_t *n, pc_error *err)
{
    *list = NULL;
    *n = 0;
    size_t cap = 0;
    char **envp = pr_lpstat_env();
    if (envp == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
    }
    /* lpstat -p -d */
    char *qargv[] = {(char *)"lpstat", (char *)"-p", (char *)"-d", NULL};
    pc_buf qout = PC_BUF_INIT;
    bool spawned = false;
    int code = -1;
    (void)pr_spawn(qargv, envp, NULL, 0, &qout, NULL, &spawned, &code, err);
    pc_status rc = PC_OK;
    if (spawned) {
        rc = pr_parse_lpstat(pc_buf_cstr(&qout), list, n, &cap, err);
    }
    char *def = (spawned && rc == PC_OK) ? pr_default_destination(pc_buf_cstr(&qout)) : NULL;
    pc_buf_fini(&qout);
    /* lpstat -e */
    if (rc == PC_OK) {
        char *eargv[] = {(char *)"lpstat", (char *)"-e", NULL};
        pc_buf eout = PC_BUF_INIT;
        bool espawn = false;
        int ecode = -1;
        (void)pr_spawn(eargv, envp, NULL, 0, &eout, NULL, &espawn, &ecode, err);
        if (espawn && ecode == 0) {
            rc = pr_parse_lpstat_e(pc_buf_cstr(&eout), def, list, n, &cap, err);
        }
        pc_buf_fini(&eout);
    }
    free(def);
    for (char **e = envp; *e != NULL; e++) {
        free(*e);
    }
    free(envp);
    if (rc != PC_OK) {
        for (size_t i = 0; i < *n; i++) {
            free((*list)[i].name);
        }
        free(*list);
        *list = NULL;
        *n = 0;
    }
    return rc;
}

/* ========================================================================================= *
 * Print commands (print_cmds::do_print / render_and_spool / remember_print / print / printOneCopy).
 * ========================================================================================= */

/* Build the command-preview array ["lp", ...] into a mutable JSON value. */
static yyjson_mut_val *pr_command_preview(yyjson_mut_doc *d, const char *printer, int copies,
                                          const char *title)
{
    size_t argn = 0;
    char **argv = pr_lp_argv(printer, copies, title, &argn);
    if (argv == NULL) {
        return NULL;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    if (arr != NULL) {
        for (size_t i = 0; i < argn; i++) {
            yyjson_mut_arr_add_strcpy(d, arr, argv[i]);
        }
    }
    pr_free_argv(argv);
    return arr;
}

/* Remember the Print settings for Print One Copy (minus output/dryRun/send). */
static void pr_remember(pc_engine_session *s, yyjson_val *p)
{
    pc_json_value tmp = PC_JSON_VALUE_INIT;
    if (pc_json_value_from_val(p, &tmp, NULL) != PC_OK) {
        return;
    }
    yyjson_mut_val *root = pc_json_value_root(&tmp);
    if (root != NULL && yyjson_mut_is_obj(root)) {
        (void)yyjson_mut_obj_remove_str(root, "output");
        (void)yyjson_mut_obj_remove_str(root, "dryRun");
        (void)yyjson_mut_obj_remove_str(root, "send");
    }
    pc_json_value_fini(&s->last_print);
    s->last_print = tmp;
}

/* Lay out, render and (unless dry-run) spool a print. Writes *out on success. Port of
 * render_and_spool. */
static pc_status pr_render_and_spool(pc_engine_session *s, const pc_document *doc, yyjson_val *p,
                                     const char *cmd, pc_json_value *out, pc_error *err)
{
    (void)s;
    pr_layout lay;
    pc_status rc = pr_compute_layout(doc, p, cmd, &lay, err);
    if (rc != PC_OK) {
        return rc;
    }
    pr_image img;
    pr_color_info color;
    rc = pr_print_image(doc, p, cmd, &img, &color, err);
    if (rc != PC_OK) {
        pr_color_fini(&color);
        return rc;
    }

    /* Label: the document name (File Info description text for the "description" field is a §7
     * deviation: only the label is surfaced). */
    char *label = NULL;
    if (pr_bool(p, "labels", false)) {
        label = pc_strndup(doc->name.ptr != NULL ? doc->name.ptr : "", doc->name.len);
    }

    pc_buf pdf = PC_BUF_INIT;
    rc = pr_print_pdf(lay.pw, lay.ph, &img, lay.x, lay.y, lay.w, lay.h, lay.marks, label, NULL,
                      lay.emulsion_down, &pdf, err);
    pr_image_fini(&img);
    free(label);
    if (rc != PC_OK) {
        pc_buf_fini(&pdf);
        pr_color_fini(&color);
        return rc;
    }

    yyjson_val *ov = pc_json_get(p, "output");
    size_t ovl = 0;
    const char *output = ov != NULL ? pc_json_as_str(ov, &ovl) : NULL;
    if (output != NULL && ovl == 0) {
        output = NULL;
    }
    bool send = pr_bool(p, "send", output == NULL);
    int64_t copies64 = 1;
    (void)pc_engine_param_int(p, "copies", &copies64);
    if (copies64 < 1) {
        copies64 = 1;
    }
    if (copies64 > 999) {
        copies64 = 999;
    }
    int copies = (int)copies64;
    bool dry = pr_bool(p, "dryRun", false);

    const char *printer = NULL;
    yyjson_val *pv = pc_json_get(p, "printer");
    size_t pl = 0;
    const char *ps = pv != NULL ? pc_json_as_str(pv, &pl) : NULL;
    if (ps != NULL && pl > 0) {
        printer = ps;
    }
    const char *title = doc->name.ptr != NULL ? doc->name.ptr : "";

    /* Write an explicit output PDF first, so it survives a spool refusal. */
    if (output != NULL) {
        rc = pc_engine_write_file(output, pdf.data, pdf.len, err);
        if (rc != PC_OK) {
            pc_buf_fini(&pdf);
            pr_color_fini(&color);
            return rc;
        }
    }

    bool sent = false;
    char *spooler = NULL;
    if (send && !dry) {
        if (pr_printing_disabled()) {
            pc_buf_fini(&pdf);
            pr_color_fini(&color);
            return pc_engine_err_other(
                err, "printing is disabled in this process (pc_engine_print_disable or "
                     "PHOTOCRAFT_NO_PRINT); use dryRun: true");
        }
        rc = pr_submit(pdf.data, pdf.len, printer, copies, title, &spooler, err);
        if (rc != PC_OK) {
            pc_buf_fini(&pdf);
            pr_color_fini(&color);
            return rc;
        }
        sent = true;
    }

    /* Build the result. */
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        free(spooler);
        pc_buf_fini(&pdf);
        pr_color_fini(&color);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_doc_set_root(jd, root);
    if (output != NULL) {
        yyjson_mut_obj_add_strcpy(jd, root, "pdf", output);
    } else {
        yyjson_mut_obj_add_null(jd, root, "pdf");
    }
    yyjson_mut_obj_add_uint(jd, root, "bytes", pdf.len);
    yyjson_mut_val *paper = yyjson_mut_arr(jd);
    yyjson_mut_arr_add_real(jd, paper, lay.pw);
    yyjson_mut_arr_add_real(jd, paper, lay.ph);
    yyjson_mut_obj_add_val(jd, root, "paper", paper);
    yyjson_mut_val *rect = yyjson_mut_arr(jd);
    yyjson_mut_arr_add_real(jd, rect, lay.x);
    yyjson_mut_arr_add_real(jd, rect, lay.y);
    yyjson_mut_arr_add_real(jd, rect, lay.w);
    yyjson_mut_arr_add_real(jd, rect, lay.h);
    yyjson_mut_obj_add_val(jd, root, "imageRect", rect);
    yyjson_mut_obj_add_bool(jd, root, "emulsionDown", lay.emulsion_down);
    yyjson_mut_obj_add_real(jd, root, "scale", lay.scale * 100.0);
    yyjson_mut_obj_add_uint(jd, root, "copies", (uint64_t)copies);
    if (send) {
        yyjson_mut_val *cmdv = pr_command_preview(jd, printer, copies, title);
        yyjson_mut_obj_add_val(jd, root, "command", cmdv != NULL ? cmdv : yyjson_mut_null(jd));
    } else {
        yyjson_mut_obj_add_null(jd, root, "command");
    }
    yyjson_mut_obj_add_bool(jd, root, "sent", sent);
    if (spooler != NULL) {
        yyjson_mut_obj_add_strcpy(jd, root, "spooler", spooler);
    } else {
        yyjson_mut_obj_add_null(jd, root, "spooler");
    }
    yyjson_mut_val *co = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_strcpy(jd, co, "colorHandling", color.handling);
    if (color.has_profile) {
        yyjson_mut_obj_add_strcpy(jd, co, "printerProfile", color.profile != NULL ? color.profile : "");
    }
    if (color.has_intent) {
        yyjson_mut_obj_add_strcpy(jd, co, "intent", color.intent);
    }
    if (color.has_bpc) {
        yyjson_mut_obj_add_bool(jd, co, "bpc", color.bpc);
    }
    yyjson_mut_obj_add_val(jd, root, "color", co);

    free(spooler);
    pc_buf_fini(&pdf);
    pr_color_fini(&color);
    return PC_OK;
}

/* Shared by Print and Print One Copy (do_print). */
static pc_status pr_do_print(pc_engine_session *s, yyjson_val *p, const char *cmd, pc_json_value *out,
                             pc_error *err)
{
    yyjson_val *ov = pc_json_get(p, "output");
    size_t ovl = 0;
    const char *output = ov != NULL ? pc_json_as_str(ov, &ovl) : NULL;
    if (output != NULL && ovl == 0) {
        output = NULL;
    }
    bool send = pr_bool(p, "send", output == NULL);
    bool dry = pr_bool(p, "dryRun", false);
    if (!dry && !send && output == NULL) {
        return pc_engine_err_bad_params(err, cmd, "send: false needs an output path or dryRun: true");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_status rc = pr_render_and_spool(s, doc, p, cmd, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    pr_remember(s, p);
    (void)pc_engine_automate_fire_event(s, "print");
    return PC_OK;
}

static pc_status run_print(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return pr_do_print(s, p, "file.print", out, err);
}

static pc_status run_print_one_copy(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                    pc_error *err)
{
    /* Merge the last Print settings with any overriding keys; force copies = 1. */
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
    }
    yyjson_mut_val *root = yyjson_mut_obj(md);
    yyjson_mut_doc_set_root(md, root);
    yyjson_mut_val *last = pc_json_value_root(&s->last_print);
    if (last != NULL && yyjson_mut_is_obj(last)) {
        yyjson_mut_obj_iter it;
        yyjson_mut_obj_iter_init(last, &it);
        yyjson_mut_val *k;
        while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
            const char *ks = yyjson_mut_get_str(k);
            yyjson_mut_val *v = yyjson_mut_obj_iter_get_val(k);
            if (ks != NULL) {
                yyjson_mut_val *vc = pc_json_normalized_copy(md, v);
                if (vc != NULL) {
                    yyjson_mut_obj_put(root, yyjson_mut_strcpy(md, ks), vc);
                }
            }
        }
    }
    if (p != NULL && yyjson_is_obj(p)) {
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(p, idx, max, k, v) {
            const char *ks = yyjson_get_str(k);
            if (ks != NULL) {
                (void)yyjson_mut_obj_remove_str(root, ks);
                yyjson_mut_val *vc = pc_json_normalized_copy_val(md, v);
                if (vc != NULL) {
                    yyjson_mut_obj_put(root, yyjson_mut_strcpy(md, ks), vc);
                }
            }
        }
    }
    (void)yyjson_mut_obj_remove_str(root, "copies");
    yyjson_mut_obj_add_uint(md, root, "copies", 1);

    yyjson_doc *imut = yyjson_mut_doc_imut_copy(md, NULL);
    yyjson_mut_doc_free(md);
    if (imut == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
    }
    yyjson_val *merged = yyjson_doc_get_root(imut);
    pc_status rc = pr_do_print(s, merged, "file.printOneCopy", out, err);
    yyjson_doc_free(imut);
    return rc;
}

static pc_status run_printers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    pr_printer *list = NULL;
    size_t n = 0;
    pc_status rc = pr_printers(&list, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        for (size_t i = 0; i < n; i++) {
            free(list[i].name);
        }
        free(list);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_doc_set_root(jd, root);
    yyjson_mut_val *arr = yyjson_mut_arr(jd);
    const char *def = NULL;
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *pe = yyjson_mut_obj(jd);
        yyjson_mut_obj_add_strcpy(jd, pe, "name", list[i].name);
        yyjson_mut_obj_add_bool(jd, pe, "default", list[i].def);
        yyjson_mut_arr_add_val(arr, pe);
        if (list[i].def) {
            def = list[i].name;
        }
    }
    yyjson_mut_obj_add_val(jd, root, "printers", arr);
    if (def != NULL) {
        yyjson_mut_obj_add_strcpy(jd, root, "default", def);
    } else {
        yyjson_mut_obj_add_null(jd, root, "default");
    }
    for (size_t i = 0; i < n; i++) {
        free(list[i].name);
    }
    free(list);
    return PC_OK;
}

/* ========================================================================================= *
 * Package (print_cmds::package): copy the document and its linked files into <dir>/<name>/.
 * ========================================================================================= */

/* The base file name (after the last '/'), as an owned string. */
static char *pr_file_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return pc_strdup(slash != NULL ? slash + 1 : path);
}

typedef struct {
    char *src;    /* original linked path */
    char *target; /* copied path under Links/ */
} pr_copied;

typedef struct {
    pc_layer_id id;
    char *target;
} pr_relink;

/* Recursively collect linked smart-object layers, copying each source once. */
static pc_status pr_collect_links(const pc_layer *layers, size_t nlayers, const char *links_dir,
                                  pr_copied **copied, size_t *ncopied, size_t *cap_copied,
                                  pr_relink **relinks, size_t *nrelinks, size_t *cap_relinks,
                                  char ***missing, size_t *nmissing, size_t *cap_missing,
                                  pc_error *err)
{
    for (size_t i = 0; i < nlayers; i++) {
        const pc_layer *l = &layers[i];
        if (pc_layer_is_group(l)) {
            size_t nc = 0;
            const pc_layer *ch = pc_layer_children(l, &nc);
            pc_status rc = pr_collect_links(ch, nc, links_dir, copied, ncopied, cap_copied, relinks,
                                            nrelinks, cap_relinks, missing, nmissing, cap_missing, err);
            if (rc != PC_OK) {
                return rc;
            }
            continue;
        }
        if (l->content.kind != PC_LAYER_SMART || !l->content.u.smart.source.linked) {
            continue;
        }
        const char *path = pc_str_c(&l->content.u.smart.source.path);
        /* Already copied? */
        char *target = NULL;
        for (size_t c = 0; c < *ncopied; c++) {
            if (strcmp((*copied)[c].src, path) == 0) {
                target = pc_strdup((*copied)[c].target);
                if (target == NULL) {
                    return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
                }
                break;
            }
        }
        if (target == NULL) {
            char *fname = pr_file_name(path);
            if (fname == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
            }
            /* Disambiguate a colliding base name. */
            bool collides = false;
            for (size_t c = 0; c < *ncopied; c++) {
                char *tn = pr_file_name((*copied)[c].target);
                bool eq = tn != NULL && strcmp(tn, fname) == 0;
                free(tn);
                if (eq) {
                    collides = true;
                    break;
                }
            }
            char *use_name = fname;
            char *prefixed = NULL;
            if (collides) {
                size_t need = strlen(fname) + 32;
                prefixed = pc_malloc_array(need, 1);
                if (prefixed == NULL) {
                    free(fname);
                    return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
                }
                (void)snprintf(prefixed, need, "%zu-%s", *ncopied, fname);
                use_name = prefixed;
            }
            char *t = pc_engine_file_join(links_dir, use_name);
            free(prefixed);
            free(fname);
            if (t == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
            }
            pc_buf bytes = PC_BUF_INIT;
            pc_status readrc = pc_engine_read_file(path, &bytes, NULL);
            if (readrc != PC_OK) {
                pc_buf_fini(&bytes);
                free(t);
                /* Missing source: record and skip. */
                char **mg = pc_grow_array(*missing, cap_missing, *nmissing + 1, sizeof **missing);
                if (mg == NULL) {
                    return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
                }
                *missing = mg;
                char *md = pc_strdup(path);
                if (md == NULL) {
                    return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
                }
                (*missing)[(*nmissing)++] = md;
                continue;
            }
            pc_status wrc = pc_engine_write_file(t, bytes.data, bytes.len, err);
            pc_buf_fini(&bytes);
            if (wrc != PC_OK) {
                free(t);
                return wrc;
            }
            pr_copied *cg = pc_grow_array(*copied, cap_copied, *ncopied + 1, sizeof **copied);
            if (cg == NULL) {
                free(t);
                return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
            }
            *copied = cg;
            (*copied)[*ncopied].src = pc_strdup(path);
            (*copied)[*ncopied].target = pc_strdup(t);
            if ((*copied)[*ncopied].src == NULL || (*copied)[*ncopied].target == NULL) {
                free((*copied)[*ncopied].src);
                free((*copied)[*ncopied].target);
                free(t);
                return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
            }
            (*ncopied)++;
            target = t;
        }
        pr_relink *rg = pc_grow_array(*relinks, cap_relinks, *nrelinks + 1, sizeof **relinks);
        if (rg == NULL) {
            free(target);
            return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
        }
        *relinks = rg;
        (*relinks)[*nrelinks].id = l->id;
        (*relinks)[*nrelinks].target = target; /* owned */
        (*nrelinks)++;
    }
    return PC_OK;
}

static pc_status run_package(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "file.package";
    yyjson_val *dv = pc_json_get(p, "dir");
    size_t dl = 0;
    const char *dir = dv != NULL ? pc_json_as_str(dv, &dl) : NULL;
    if (dir == NULL || dl == 0) {
        return pc_engine_err_bad_params(err, cmd, "missing \"dir\" (where to create the package folder)");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);

    const char *ext = "pcraft";
    yyjson_val *fv = pc_json_get(p, "format");
    size_t fl = 0;
    const char *fs = fv != NULL ? pc_json_as_str(fv, &fl) : NULL;
    if (fs != NULL) {
        if (strcmp(fs, "psd") == 0 || strcmp(fs, "psb") == 0 || strcmp(fs, "pcraft") == 0) {
            ext = fs;
        } else {
            return pc_engine_err_bad_params(err, cmd, "unknown package format `%s` (pcraft|psd|psb)", fs);
        }
    }

    /* name = stem(path or document name). */
    char stem[512];
    pc_engine_file_stem(d->path != NULL ? d->path : pc_str_c(&doc->name), stem, sizeof stem);

    pc_buf san = PC_BUF_INIT;
    pc_status rc = pc_engine_file_sanitize(&san, stem, err);
    if (rc != PC_OK) {
        pc_buf_fini(&san);
        return rc;
    }
    char *folder = pc_engine_file_join(dir, pc_buf_cstr(&san));
    if (folder == NULL) {
        pc_buf_fini(&san);
        return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
    }
    char *links_dir = pc_engine_file_join(folder, "Links");
    if (links_dir == NULL) {
        free(folder);
        pc_buf_fini(&san);
        return pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
    }

    pr_copied *copied = NULL;
    size_t ncopied = 0, cap_copied = 0;
    pr_relink *relinks = NULL;
    size_t nrelinks = 0, cap_relinks = 0;
    char **missing = NULL;
    size_t nmissing = 0, cap_missing = 0;
    rc = pr_collect_links(doc->layers, doc->nlayers, links_dir, &copied, &ncopied, &cap_copied,
                          &relinks, &nrelinks, &cap_relinks, &missing, &nmissing, &cap_missing, err);
    free(links_dir);
    if (rc != PC_OK) {
        goto cleanup;
    }

    /* Relink the copy of the document to the copies (on a scratch session). */
    pc_engine_session *t = NULL;
    rc = pc_engine_session_new(&t, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    pc_document clone;
    rc = pc_document_clone(doc, &clone, err);
    if (rc != PC_OK) {
        pc_engine_session_free(t);
        goto cleanup;
    }
    size_t idx = 0;
    rc = pc_engine_session_add_document(t, &clone, NULL, &idx, err);
    if (rc != PC_OK) {
        pc_document_fini(&clone);
        pc_engine_session_free(t);
        goto cleanup;
    }
    for (size_t i = 0; i < nrelinks && rc == PC_OK; i++) {
        yyjson_mut_doc *rp = yyjson_mut_doc_new(NULL);
        if (rp == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
            break;
        }
        yyjson_mut_val *ro = yyjson_mut_obj(rp);
        yyjson_mut_doc_set_root(rp, ro);
        yyjson_mut_obj_add_uint(rp, ro, "layer", relinks[i].id);
        yyjson_mut_obj_add_strcpy(rp, ro, "path", relinks[i].target);
        rc = pr_exec(t, "layer.smartObjects.relinkToFile", rp, err);
    }
    if (rc != PC_OK) {
        pc_engine_session_free(t);
        goto cleanup;
    }

    /* Save the relinked document. */
    size_t need = strlen(pc_buf_cstr(&san)) + strlen(ext) + 2;
    char *docname = pc_malloc_array(need, 1);
    if (docname == NULL) {
        pc_engine_session_free(t);
        rc = pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
        goto cleanup;
    }
    (void)snprintf(docname, need, "%s.%s", pc_buf_cstr(&san), ext);
    char *outpath = pc_engine_file_join(folder, docname);
    free(docname);
    if (outpath == NULL) {
        pc_engine_session_free(t);
        rc = pc_fail(err, PC_ERR_NOMEM, "print: out of memory");
        goto cleanup;
    }
    const pc_engine_doc_state *ds = pc_engine_session_active(t);
    if (ds == NULL) {
        free(outpath);
        pc_engine_session_free(t);
        rc = pc_engine_err_no_document(err);
        goto cleanup;
    }
    rc = pc_engine_save_doc(pc_doc_arc_get(ds->doc), outpath, NULL, NULL, err);
    pc_engine_session_free(t);
    if (rc != PC_OK) {
        free(outpath);
        goto cleanup;
    }

    /* Result. */
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        free(outpath);
        rc = err != NULL ? err->code : PC_ERR_NOMEM;
        goto cleanup;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_doc_set_root(jd, root);
    yyjson_mut_obj_add_strcpy(jd, root, "folder", folder);
    yyjson_mut_obj_add_strcpy(jd, root, "document", outpath);
    yyjson_mut_val *links = yyjson_mut_arr(jd);
    for (size_t i = 0; i < ncopied; i++) {
        yyjson_mut_arr_add_strcpy(jd, links, copied[i].target);
    }
    yyjson_mut_obj_add_val(jd, root, "links", links);
    yyjson_mut_val *miss = yyjson_mut_arr(jd);
    for (size_t i = 0; i < nmissing; i++) {
        yyjson_mut_arr_add_strcpy(jd, miss, missing[i]);
    }
    yyjson_mut_obj_add_val(jd, root, "missing", miss);
    yyjson_mut_val *warn = yyjson_mut_arr(jd);
    yyjson_mut_obj_add_val(jd, root, "warnings", warn);
    free(outpath);

cleanup:
    for (size_t i = 0; i < ncopied; i++) {
        free(copied[i].src);
        free(copied[i].target);
    }
    free(copied);
    for (size_t i = 0; i < nrelinks; i++) {
        free(relinks[i].target);
    }
    free(relinks);
    for (size_t i = 0; i < nmissing; i++) {
        free(missing[i]);
    }
    free(missing);
    free(folder);
    pc_buf_fini(&san);
    return rc;
}

/* ========================================================================================= *
 * Registry.
 * ========================================================================================= */

static const char *const MENU_FILE[] = {"File"};
static const char *const MENU_FILE_EXPORT[] = {"File", "Export"};

#define PR_PRINT_PARAMS                                                                            \
    "{\"printer\":name?,\"copies\":1..999=1,\"paper\":\"" PR_PAPER_NAMES "\"|[w,h] "               \
    "pt=\"letter\",\"orientation\":\"portrait|landscape\"=\"portrait\",\"center\":bool=true,"     \
    "\"top\":in?,\"left\":in?,\"scale\":%=100,\"scaleToFit\":bool=false,\"colorHandling\":"        \
    "\"printerManages|photocraftManages|noColorManagement\"=\"printerManages\",\"printerProfile\":"\
    "profile?,\"intent\":\"perceptual|relative|saturation|absolute\"=\"relative\",\"bpc\":bool="   \
    "true,\"cornerCropMarks\":bool,\"centerCropMarks\":bool,\"registrationMarks\":bool,"           \
    "\"description\":bool,\"labels\":bool,\"emulsionDown\":bool=false,\"output\":pdf path?,"       \
    "\"send\":bool?,\"dryRun\":bool=false} -> {pdf, imageRect, command, sent}"

const pc_engine_command *pc_engine_print_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"file.print", "Print\xe2\x80\xa6", MENU_FILE, 1, "Cmd+P", PR_PRINT_PARAMS, pc_engine_has_doc,
         run_print, true},
        {"file.printers", "Printers", NULL, 0, NULL,
         "{} -> {printers:[{name, default}], default:name|null} (the system spooler's printers)",
         pc_engine_always, run_printers, false},
        {"file.printOneCopy", "Print One Copy", MENU_FILE, 1, "Cmd+Alt+Shift+P",
         "{} (the last Print settings, one copy; any Print key overrides)", pc_engine_has_doc,
         run_print_one_copy, true},
        {"file.package", "Package\xe2\x80\xa6", MENU_FILE, 1, NULL,
         "{\"dir\":folder,\"format\":\"pcraft|psd|psb\"=\"pcraft\"} -> {folder, document, links, "
         "missing} (copies the document and its linked files into <dir>/<name>/, relinked to Links/)",
         pc_engine_has_doc, run_package, true},
        {"file.export.pathsToIllustrator", "Paths to Illustrator\xe2\x80\xa6", MENU_FILE_EXPORT, 2,
         NULL,
         "{\"path\":str? (.ai; omit to return the text),\"paths\":\"all|work|<path name>\"=\"all\"}"
         " -> {path, paths}",
         pr_has_paths, run_paths_to_illustrator, true},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
