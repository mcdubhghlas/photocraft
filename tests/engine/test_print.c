/* File > Print, Print One Copy, Printers, Package and Export > Paths to Illustrator (print_cmds.c).
 * Port of crates/engine/src/print_cmds/tests.rs plus graceful-failure coverage for the POSIX
 * spooler path (no printer, bad params, a fake `lp` transport).
 *
 * Spooling is blocked for the whole process by setting PHOTOCRAFT_NO_PRINT in main(), so no test
 * reaches a real printer. The transport test temporarily unsets it and puts a fake `lp` on PATH. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/buf.h"
#include "base/compress.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "harness.h"

/* ---- small helpers ---- */

static pc_engine_session *new_session(void)
{
    pc_engine_session *s = NULL;
    if (pc_engine_session_new(&s, NULL) != PC_OK) {
        return NULL;
    }
    return s;
}

/* file.new of a 300x150 document in `mode` at `depth` with a blue background. */
static pc_engine_session *doc_session(const char *mode, int depth)
{
    pc_engine_session *s = new_session();
    if (s == NULL) {
        return NULL;
    }
    char j[256];
    (void)snprintf(j, sizeof j,
                   "{\"width\":300,\"height\":150,\"mode\":\"%s\",\"depth\":%d,\"name\":\"Print "
                   "Me.psd\",\"background\":\"#3366cc\"}",
                   mode, depth);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, "file.new", j, &out, NULL);
    pc_json_value_fini(&out);
    if (rc != PC_OK) {
        pc_engine_session_free(s);
        return NULL;
    }
    return s;
}

/* Execute, returning the status; frees the result. */
static pc_status ex(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    pc_error err = {0};
    pc_json_value tmp = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, out != NULL ? out : &tmp, &err);
    if (out == NULL) {
        pc_json_value_fini(&tmp);
    }
    return rc;
}

static yyjson_mut_val *obj_get(pc_json_value *v, const char *key)
{
    return yyjson_mut_obj_get(pc_json_value_root(v), key);
}

static size_t mem_find(const uint8_t *h, size_t hl, const char *n, size_t from)
{
    size_t nl = strlen(n);
    if (nl == 0 || hl < nl) {
        return SIZE_MAX;
    }
    for (size_t i = from; i + nl <= hl; i++) {
        if (memcmp(h + i, n, nl) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static int read_file_bytes(const char *path, pc_buf *out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    pc_buf_init(out);
    uint8_t chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        (void)pc_buf_append(out, chunk, got, NULL);
    }
    fclose(f);
    return 1;
}

/* A decimal number right after `key` in a text region. */
static long num_after(const uint8_t *b, size_t len, const char *key, size_t from)
{
    size_t i = mem_find(b, len, key, from);
    if (i == SIZE_MAX) {
        return -1;
    }
    i += strlen(key);
    return strtol((const char *)b + i, NULL, 10);
}

/* The inflated image stream of a print PDF: width, height, colour-space token, samples. */
typedef struct {
    uint32_t w, h;
    char cs[64];
    pc_buf data;
} pdf_image;

static int extract_image(const uint8_t *pdf, size_t len, pdf_image *im)
{
    memset(im, 0, sizeof *im);
    size_t i = mem_find(pdf, len, "/Subtype /Image", 0);
    if (i == SIZE_MAX) {
        return 0;
    }
    size_t end = mem_find(pdf, len, ">>\nstream\n", i);
    if (end == SIZE_MAX) {
        return 0;
    }
    im->w = (uint32_t)num_after(pdf, len, "/Width ", i);
    im->h = (uint32_t)num_after(pdf, len, "/Height ", i);
    long length = num_after(pdf, len, "/Length ", i);
    /* ColorSpace token up to " /BitsPerComponent". */
    size_t cs = mem_find(pdf, len, "/ColorSpace ", i);
    size_t bpc = mem_find(pdf, len, " /BitsPerComponent", i);
    if (cs != SIZE_MAX && bpc != SIZE_MAX && bpc > cs) {
        cs += strlen("/ColorSpace ");
        size_t n = bpc - cs;
        if (n >= sizeof im->cs) {
            n = sizeof im->cs - 1;
        }
        memcpy(im->cs, pdf + cs, n);
        im->cs[n] = '\0';
    }
    if (length < 0) {
        return 0;
    }
    size_t start = end + 10;
    if (start + (size_t)length > len) {
        return 0;
    }
    pc_buf_init(&im->data);
    return pc_inflate(PC_Z_ZLIB, pdf + start, (size_t)length, (size_t)64 << 20, &im->data, NULL,
                      NULL) == PC_OK;
}

/* The inflated page content stream (object 4). */
static int extract_content(const uint8_t *pdf, size_t len, pc_buf *out)
{
    size_t i = mem_find(pdf, len, "4 0 obj\n", 0);
    if (i == SIZE_MAX) {
        return 0;
    }
    long length = num_after(pdf, len, "/Length ", i);
    size_t st = mem_find(pdf, len, "stream\n", i);
    if (length < 0 || st == SIZE_MAX) {
        return 0;
    }
    size_t start = st + 7;
    if (start + (size_t)length > len) {
        return 0;
    }
    pc_buf_init(out);
    return pc_inflate(PC_Z_ZLIB, pdf + start, (size_t)length, (size_t)64 << 20, out, NULL, NULL) ==
           PC_OK;
}

static const char *g_tmp; /* set in main (TMPDIR or /tmp) */

static char *tmp_path(const char *name)
{
    size_t n = strlen(g_tmp) + strlen(name) + 32;
    char *p = malloc(n);
    if (p != NULL) {
        (void)snprintf(p, n, "%s/pc-print-%ld-%s", g_tmp, (long)getpid(), name);
    }
    return p;
}

static void rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            size_t n = strlen(path) + strlen(e->d_name) + 2;
            char *child = malloc(n);
            if (child != NULL) {
                (void)snprintf(child, n, "%s/%s", path, e->d_name);
                rm_rf(child);
                free(child);
            }
        }
        closedir(d);
    }
    (void)remove(path);
}

/* ---- tests ---- */

static void test_dry_run_renders_and_reports_backend(void)
{
    for (int di = 0; di < 3; di++) {
        int depth = (int[]){8, 16, 32}[di];
        pc_engine_session *s = doc_session("rgb", depth);
        CHECK(s != NULL);
        char *out = tmp_path("rgb.pdf");
        char j[512];
        (void)snprintf(j, sizeof j,
                       "{\"output\":\"%s\",\"send\":true,\"dryRun\":true,\"printer\":\"Office\","
                       "\"copies\":2,\"paper\":\"a4\",\"cornerCropMarks\":true,"
                       "\"registrationMarks\":true,\"labels\":true}",
                       out);
        pc_json_value r = PC_JSON_VALUE_INIT;
        pc_status rc = ex(s, "file.print", j, &r);
        CHECK(rc == PC_OK);
        yyjson_mut_val *cmd = obj_get(&r, "command");
        CHECK(cmd != NULL && yyjson_mut_is_arr(cmd));
        const char *want[5] = {"lp", "-d", "Office", "-n", "2"};
        for (int k = 0; k < 5; k++) {
            CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_arr_get(cmd, (size_t)k)), want[k]);
        }
        bool has_fit = false, has_out = false;
        size_t idx, max;
        yyjson_mut_val *el;
        yyjson_mut_arr_foreach(cmd, idx, max, el) {
            const char *v = yyjson_mut_get_str(el);
            if (v != NULL && strcmp(v, "fit-to-page=false") == 0) {
                has_fit = true;
            }
            if (v != NULL && strcmp(v, out) == 0) {
                has_out = true;
            }
        }
        CHECK(has_fit);
        CHECK(!has_out); /* PDF streamed on stdin, not a path argument */
        CHECK(yyjson_mut_get_bool(obj_get(&r, "sent")) == false);
        CHECK_EQ_STR(yyjson_mut_get_str(obj_get(&r, "pdf")), out);
        pc_json_value_fini(&r);

        pc_buf pdf = PC_BUF_INIT;
        CHECK(read_file_bytes(out, &pdf));
        CHECK(pdf.len > 8 && memcmp(pdf.data, "%PDF-1.4", 8) == 0);
        CHECK(mem_find(pdf.data, pdf.len, "/MediaBox [0 0 595.28 841.89]", 0) != SIZE_MAX);
        pdf_image im;
        CHECK(extract_image(pdf.data, pdf.len, &im));
        CHECK_EQ_UINT(im.w, 300u);
        CHECK_EQ_UINT(im.h, 150u);
        CHECK_EQ_STR(im.cs, "/DeviceRGB");
        CHECK(im.data.len >= 3 && im.data.data[0] == 0x33 && im.data.data[1] == 0x66 &&
                    im.data.data[2] == 0xcc);
        pc_buf_fini(&im.data);
        pc_buf_fini(&pdf);
        (void)remove(out);
        free(out);
        pc_engine_session_free(s);
    }

    /* Print to PDF: no spooling by default, gray + scale-to-fit landscape. */
    pc_engine_session *s = doc_session("gray", 8);
    CHECK(s != NULL);
    char *out = tmp_path("out.pdf");
    char j[512];
    (void)snprintf(j, sizeof j, "{\"output\":\"%s\",\"scaleToFit\":true,\"orientation\":\"landscape\"}",
                   out);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.print", j, &r) == PC_OK);
    CHECK(yyjson_mut_get_bool(obj_get(&r, "sent")) == false);
    CHECK(yyjson_mut_is_null(obj_get(&r, "command")));
    double scale = yyjson_mut_get_num(obj_get(&r, "scale"));
    CHECK(scale > 200.0);
    pc_json_value_fini(&r);
    pc_buf pdf = PC_BUF_INIT;
    CHECK(read_file_bytes(out, &pdf));
    pdf_image im;
    CHECK(extract_image(pdf.data, pdf.len, &im));
    CHECK_EQ_STR(im.cs, "/DeviceGray");
    pc_buf_fini(&im.data);
    pc_buf_fini(&pdf);
    (void)remove(out);
    free(out);

    /* Bad paper and missing printer profile are errors. */
    CHECK(ex(s, "file.print", "{\"paper\":\"napkin\",\"dryRun\":true}", NULL) != PC_OK);
    CHECK(ex(s, "file.print", "{\"colorHandling\":\"photocraftManages\",\"dryRun\":true}",
                   NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_photocraft_manages_converts_to_printer_profile(void)
{
    pc_engine_session *s = doc_session("rgb", 8);
    CHECK(s != NULL);
    char *out = tmp_path("managed.pdf");
    char j[512];
    (void)snprintf(j, sizeof j,
                   "{\"output\":\"%s\",\"dryRun\":true,\"colorHandling\":\"photocraftManages\","
                   "\"printerProfile\":\"coated-cmyk\",\"intent\":\"perceptual\",\"bpc\":false}",
                   out);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.print", j, &r) == PC_OK);
    yyjson_mut_val *color = obj_get(&r, "color");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(color, "intent")), "perceptual");
    pc_json_value_fini(&r);

    pc_buf pdf = PC_BUF_INIT;
    CHECK(read_file_bytes(out, &pdf));
    pdf_image im;
    CHECK(extract_image(pdf.data, pdf.len, &im));
    CHECK_EQ_STR(im.cs, "[/ICCBased 7 0 R]");
    CHECK(mem_find(pdf.data, pdf.len, "/N 4 /Alternate /DeviceCMYK", 0) != SIZE_MAX);
    CHECK_EQ_UINT(im.data.len, (size_t)300 * 150 * 4);
    CHECK(im.data.data[0] > 150 && im.data.data[2] < 60); /* blue: lots of cyan, little yellow */
    pc_buf_fini(&im.data);
    pc_buf_fini(&pdf);
    (void)remove(out);
    free(out);

    /* The document itself is untouched (still RGB). */
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(pc_doc_arc_get(d->doc)->mode == PC_COLOR_MODE_RGB);
    pc_engine_session_free(s);
}

static void test_printer_profile_from_icc_file(void)
{
    pc_engine_session *s = doc_session("rgb", 16);
    CHECK(s != NULL);
    char *out = tmp_path("icc.pdf");
    /* The shipped coated-CMYK profile on disk stands in for an installed .icc file. */
    char j[768];
    (void)snprintf(j, sizeof j,
                   "{\"output\":\"%s\",\"dryRun\":true,\"colorHandling\":\"photocraftManages\","
                   "\"printerProfile\":\"data/profiles/photocraft-coated-cmyk.icc\","
                   "\"emulsionDown\":true}",
                   out);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.print", j, &r) == PC_OK);
    yyjson_mut_val *color = obj_get(&r, "color");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(color, "printerProfile")),
                "data/profiles/photocraft-coated-cmyk.icc");
    pc_json_value_fini(&r);
    pc_buf pdf = PC_BUF_INIT;
    CHECK(read_file_bytes(out, &pdf));
    pdf_image im;
    CHECK(extract_image(pdf.data, pdf.len, &im));
    CHECK_EQ_STR(im.cs, "[/ICCBased 7 0 R]");
    CHECK_EQ_UINT(im.data.len, (size_t)300 * 150 * 4);
    pc_buf_fini(&im.data);
    pc_buf_fini(&pdf);
    (void)remove(out);
    free(out);

    /* A missing profile file is an error. */
    CHECK(ex(s, "file.print",
                   "{\"dryRun\":true,\"colorHandling\":\"photocraftManages\",\"printerProfile\":"
                   "\"/no/such/missing.icc\"}",
                   NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_emulsion_down_mirrors_the_page(void)
{
    pc_engine_session *s = doc_session("rgb", 8);
    CHECK(s != NULL);
    char *plain_p = tmp_path("plain.pdf");
    char *mirror_p = tmp_path("mirror.pdf");
    char j[512];
    (void)snprintf(j, sizeof j,
                   "{\"output\":\"%s\",\"dryRun\":true,\"paper\":\"a4\",\"center\":false,"
                   "\"left\":1.0,\"top\":1.0,\"labels\":true}",
                   plain_p);
    pc_json_value rp = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.print", j, &rp) == PC_OK);
    (void)snprintf(j, sizeof j,
                   "{\"output\":\"%s\",\"dryRun\":true,\"paper\":\"a4\",\"center\":false,"
                   "\"left\":1.0,\"top\":1.0,\"labels\":true,\"emulsionDown\":true}",
                   mirror_p);
    pc_json_value rm = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.print", j, &rm) == PC_OK);
    CHECK(yyjson_mut_get_bool(obj_get(&rp, "emulsionDown")) == false);
    CHECK(yyjson_mut_get_bool(obj_get(&rm, "emulsionDown")) == true);
    /* Same layout. */
    yyjson_mut_val *ra = obj_get(&rp, "imageRect");
    yyjson_mut_val *rb = obj_get(&rm, "imageRect");
    for (size_t k = 0; k < 4; k++) {
        CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(ra, k)),
                 yyjson_mut_get_num(yyjson_mut_arr_get(rb, k)), 1e-9);
    }
    pc_json_value_fini(&rp);
    pc_json_value_fini(&rm);

    pc_buf p = PC_BUF_INIT, m = PC_BUF_INIT;
    CHECK(read_file_bytes(plain_p, &p) && read_file_bytes(mirror_p, &m));
    pc_buf pc = PC_BUF_INIT, mc = PC_BUF_INIT;
    CHECK(extract_content(p.data, p.len, &pc));
    CHECK(extract_content(m.data, m.len, &mc));
    CHECK(mem_find(pc.data, pc.len, "-1 0 0 1", 0) == SIZE_MAX);
    size_t flip = mem_find(mc.data, mc.len, "q -1 0 0 1 595.28 0 cm", 0);
    CHECK(flip != SIZE_MAX);
    CHECK(flip > mem_find(mc.data, mc.len, "re f Q", 0));
    CHECK(flip < mem_find(mc.data, mc.len, "/Im0 Do", 0));
    pc_buf_fini(&pc);
    pc_buf_fini(&mc);
    /* The pixels themselves are not touched (the transform mirrors them). */
    pdf_image ia, ib;
    CHECK(extract_image(p.data, p.len, &ia) && extract_image(m.data, m.len, &ib));
    CHECK_EQ_UINT(ia.data.len, ib.data.len);
    CHECK(ia.data.len == 0 || memcmp(ia.data.data, ib.data.data, ia.data.len) == 0);
    pc_buf_fini(&ia.data);
    pc_buf_fini(&ib.data);
    pc_buf_fini(&p);
    pc_buf_fini(&m);
    (void)remove(plain_p);
    (void)remove(mirror_p);

    /* Print One Copy keeps Emulsion Down. */
    char *again_p = tmp_path("again.pdf");
    (void)snprintf(j, sizeof j, "{\"output\":\"%s\",\"dryRun\":true}", again_p);
    pc_json_value ra2 = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.printOneCopy", j, &ra2) == PC_OK);
    CHECK(yyjson_mut_get_bool(obj_get(&ra2, "emulsionDown")) == true);
    pc_json_value_fini(&ra2);
    (void)remove(again_p);
    free(again_p);
    free(plain_p);
    free(mirror_p);
    pc_engine_session_free(s);
}

static void test_print_one_copy_repeats_last_settings(void)
{
    pc_engine_session *s = doc_session("rgb", 8);
    CHECK(s != NULL);
    pc_json_value first = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.print", "{\"dryRun\":true,\"copies\":3,\"paper\":\"a5\",\"scale\":50}",
                   &first) == PC_OK);
    CHECK(yyjson_mut_is_null(obj_get(&first, "pdf")));
    pc_json_value_fini(&first);

    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.printOneCopy", "{\"dryRun\":true}", &r) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_num(obj_get(&r, "copies")), 1u);
    yyjson_mut_val *paper = obj_get(&r, "paper");
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(paper, 0)), 419.53, 1e-6);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_arr_get(paper, 1)), 595.28, 1e-6);
    CHECK_NEAR(yyjson_mut_get_num(obj_get(&r, "scale")), 50.0, 1e-9);
    yyjson_mut_val *cmd = obj_get(&r, "command");
    const char *w3[3] = {"lp", "-n", "1"};
    for (int k = 0; k < 3; k++) {
        CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_arr_get(cmd, (size_t)k)), w3[k]);
    }
    CHECK(yyjson_mut_is_null(obj_get(&r, "pdf")));
    pc_json_value_fini(&r);
    pc_engine_session_free(s);
}

static void test_dry_run_without_output_retains_no_pdf(void)
{
    pc_engine_session *s = doc_session("rgb", 8);
    CHECK(s != NULL);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.print", "{\"dryRun\":true}", &r) == PC_OK);
    CHECK(yyjson_mut_is_null(obj_get(&r, "pdf")));
    CHECK(yyjson_mut_get_bool(obj_get(&r, "sent")) == false);
    CHECK((uint64_t)yyjson_mut_get_num(obj_get(&r, "bytes")) > 100);
    CHECK(yyjson_mut_is_arr(obj_get(&r, "command")));
    pc_json_value_fini(&r);
    pc_engine_session_free(s);
}

static void test_non_sending_requires_output_and_forgets_send(void)
{
    pc_engine_session *s = doc_session("rgb", 8);
    CHECK(s != NULL);
    pc_error err = {0};
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, "file.print", "{\"send\":false}", &out, &err);
    CHECK(rc != PC_OK);
    CHECK_CONTAINS(err.msg, "needs an output path");
    pc_json_value_fini(&out);

    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.print", "{\"send\":false,\"dryRun\":true}", &r) == PC_OK);
    CHECK(yyjson_mut_is_null(obj_get(&r, "pdf")));
    CHECK(yyjson_mut_is_null(obj_get(&r, "command")));
    pc_json_value_fini(&r);

    char *out_p = tmp_path("export.pdf");
    char j[512];
    (void)snprintf(j, sizeof j, "{\"output\":\"%s\",\"send\":false}", out_p);
    pc_json_value r2 = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.print", j, &r2) == PC_OK);
    CHECK(yyjson_mut_get_bool(obj_get(&r2, "sent")) == false);
    pc_json_value_fini(&r2);
    pc_buf pdf = PC_BUF_INIT;
    CHECK(read_file_bytes(out_p, &pdf));
    CHECK(pdf.len > 8 && memcmp(pdf.data, "%PDF-1.4", 8) == 0);
    pc_buf_fini(&pdf);
    (void)remove(out_p);
    free(out_p);
    pc_engine_session_free(s);
}

/* PHOTOCRAFT_NO_PRINT (set in main) blocks every real spool; a dry run still reports the command. */
static void test_disabled_printing_never_spools(void)
{
    pc_engine_session *s = new_session();
    CHECK(s != NULL);
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}", NULL) == PC_OK);
    pc_error err = {0};
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, "file.print", "{}", &out, &err);
    CHECK(rc != PC_OK);
    CHECK_CONTAINS(err.msg, "printing is disabled");
    pc_json_value_fini(&out);
    CHECK(ex(s, "file.printOneCopy", "{}", NULL) != PC_OK);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.print", "{\"dryRun\":true}", &r) == PC_OK);
    CHECK(yyjson_mut_get_bool(obj_get(&r, "sent")) == false);
    CHECK(yyjson_mut_is_arr(obj_get(&r, "command")));
    pc_json_value_fini(&r);
    pc_engine_session_free(s);
}

static void test_printers_lists_destinations(void)
{
    pc_engine_session *s = new_session();
    CHECK(s != NULL);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.printers", "{}", &r) == PC_OK); /* no document needed */
    yyjson_mut_val *list = obj_get(&r, "printers");
    CHECK(list != NULL && yyjson_mut_is_arr(list));
    size_t idx, max;
    yyjson_mut_val *el;
    yyjson_mut_arr_foreach(list, idx, max, el) {
        CHECK(yyjson_mut_is_str(yyjson_mut_obj_get(el, "name")));
        CHECK(yyjson_mut_is_bool(yyjson_mut_obj_get(el, "default")));
    }
    yyjson_mut_val *def = obj_get(&r, "default");
    CHECK(def != NULL && (yyjson_mut_is_null(def) || yyjson_mut_is_str(def)));
    pc_json_value_fini(&r);
    pc_engine_session_free(s);
}

/* A fake `lp` on PATH streams the PDF on stdin and reports a job id; a refusal propagates. */
static void test_spool_transport_with_fake_lp(void)
{
    char dirtmpl[] = "./pc-lp-XXXXXX"; /* exec-capable (root disk), not /dev/shm (noexec) */
    char *dir = mkdtemp(dirtmpl);
    if (dir == NULL) {
        return; /* can't create a temp dir here; skip */
    }
    char script[256], received[256], args[256];
    (void)snprintf(script, sizeof script, "%s/lp", dir);
    (void)snprintf(received, sizeof received, "%s/received.pdf", dir);
    (void)snprintf(args, sizeof args, "%s/args", dir);
    FILE *f = fopen(script, "w");
    CHECK(f != NULL);
    (void)fprintf(f,
                  "#!/bin/sh\n"
                  "for a in \"$@\"; do if [ \"$a\" = Refuse ]; then echo 'queue refused' >&2; "
                  "exit 1; fi; done\n"
                  "printf '%%s\\n' \"$@\" > \"%s\"\n"
                  "cat > \"%s\"\n"
                  "echo 'request id is Office-42 (0 file(s))'\n",
                  args, received);
    fclose(f);
    (void)chmod(script, 0700);

    /* Put the fake lp first on PATH and allow spooling just for this test. */
    char *old_path = getenv("PATH");
    char *saved_path = old_path != NULL ? strdup(old_path) : NULL;
    char newpath[2048];
    (void)snprintf(newpath, sizeof newpath, "%s:%s", dir, saved_path != NULL ? saved_path : "");
    setenv("PATH", newpath, 1);
    unsetenv("PHOTOCRAFT_NO_PRINT");

    pc_engine_session *s = doc_session("rgb", 8);
    CHECK(s != NULL);
    pc_json_value r = PC_JSON_VALUE_INIT;
    pc_status rc = ex(s, "file.print",
                      "{\"printer\":\"Office\",\"copies\":3,\"paper\":\"a4\",\"scale\":50}", &r);
    CHECK(rc == PC_OK);
    CHECK(yyjson_mut_get_bool(obj_get(&r, "sent")) == true);
    CHECK_EQ_STR(yyjson_mut_get_str(obj_get(&r, "spooler")), "request id is Office-42");
    CHECK(yyjson_mut_is_null(obj_get(&r, "pdf"))); /* spool-only: no diagnostic PDF */
    pc_json_value_fini(&r);

    pc_buf got = PC_BUF_INIT;
    CHECK(read_file_bytes(received, &got));
    CHECK(got.len > 8 && memcmp(got.data, "%PDF-1.4", 8) == 0);
    CHECK(mem_find(got.data, got.len, "/MediaBox [0 0 595.28 841.89]", 0) != SIZE_MAX);
    pc_buf_fini(&got);
    pc_buf argbuf = PC_BUF_INIT;
    CHECK(read_file_bytes(args, &argbuf));
    CHECK(mem_find(argbuf.data, argbuf.len, "fit-to-page=false", 0) != SIZE_MAX);
    CHECK(mem_find(argbuf.data, argbuf.len, "-n\n3\n", 0) != SIZE_MAX);
    CHECK(mem_find(argbuf.data, argbuf.len, ".pdf", 0) == SIZE_MAX);
    pc_buf_fini(&argbuf);

    /* A refusal propagates, and an explicit export survives it. */
    char *exp = tmp_path("saved.pdf");
    char j[512];
    (void)snprintf(j, sizeof j, "{\"printer\":\"Refuse\",\"output\":\"%s\",\"send\":true}", exp);
    pc_error err = {0};
    pc_json_value o = PC_JSON_VALUE_INIT;
    pc_status rc2 = pc_engine_session_execute_json(s, "file.print", j, &o, &err);
    CHECK(rc2 != PC_OK);
    CHECK_CONTAINS(err.msg, "queue refused");
    pc_json_value_fini(&o);
    pc_buf saved = PC_BUF_INIT;
    CHECK(read_file_bytes(exp, &saved));
    CHECK(saved.len > 8 && memcmp(saved.data, "%PDF-1.4", 8) == 0);
    pc_buf_fini(&saved);
    (void)remove(exp);
    free(exp);
    pc_engine_session_free(s);

    /* Restore. */
    if (saved_path != NULL) {
        setenv("PATH", saved_path, 1);
        free(saved_path);
    } else {
        unsetenv("PATH");
    }
    setenv("PHOTOCRAFT_NO_PRINT", "1", 1);
    rm_rf(dir);
}

static void test_package_copies_links_and_relinks(void)
{
    char base[256];
    (void)snprintf(base, sizeof base, "%s/pc-pkg-%ld-XXXXXX", g_tmp, (long)getpid());
    if (mkdtemp(base) == NULL) {
        return;
    }
    char linkbuf[320], pkgbuf[320];
    (void)snprintf(linkbuf, sizeof linkbuf, "%s/art.png", base);
    (void)snprintf(pkgbuf, sizeof pkgbuf, "%s/pkg", base);
    char *link = linkbuf;
    char *pkg = pkgbuf;
    /* Make a small red PNG to link. */
    pc_engine_session *src = new_session();
    CHECK(src != NULL);
    CHECK(ex(src, "file.new", "{\"width\":20,\"height\":20,\"background\":\"#ff0000\"}", NULL) ==
                PC_OK);
    char jsave[512];
    (void)snprintf(jsave, sizeof jsave, "{\"path\":\"%s\"}", link);
    CHECK(ex(src, "file.saveACopy", jsave, NULL) == PC_OK);
    pc_engine_session_free(src);

    pc_engine_session *s = doc_session("rgb", 8);
    CHECK(s != NULL);
    char jplace[512];
    (void)snprintf(jplace, sizeof jplace, "{\"path\":\"%s\"}", link);
    CHECK(ex(s, "file.placeLinked", jplace, NULL) == PC_OK);
    char jpkg[512];
    (void)snprintf(jpkg, sizeof jpkg, "{\"dir\":\"%s\"}", pkg);
    pc_json_value r = PC_JSON_VALUE_INIT;
    pc_status rc = ex(s, "file.package", jpkg, &r);
    CHECK(rc == PC_OK);
    yyjson_mut_val *links = obj_get(&r, "links");
    CHECK(links != NULL && yyjson_mut_arr_size(links) == 1);
    const char *copied = yyjson_mut_get_str(yyjson_mut_arr_get(links, 0));
    CHECK(copied != NULL);
    CHECK_CONTAINS(copied, "Print Me/Links/art.png");
    struct stat st;
    CHECK(stat(copied, &st) == 0); /* the copy exists */
    const char *docp = yyjson_mut_get_str(obj_get(&r, "document"));
    CHECK(docp != NULL);
    CHECK_CONTAINS(docp, "Print Me/Print Me.pcraft");
    pc_json_value_fini(&r);

    /* The open document still points at the original link. */
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    bool points_original = false;
    for (size_t i = 0; i < doc->nlayers; i++) {
        const pc_layer *l = &doc->layers[i];
        if (l->content.kind == PC_LAYER_SMART && l->content.u.smart.source.linked) {
            const char *p = l->content.u.smart.source.path.ptr;
            if (p != NULL && strcmp(p, link) == 0) {
                points_original = true;
            }
        }
    }
    CHECK(points_original);
    pc_engine_session_free(s);

    rm_rf(base);
}

static void test_paths_to_illustrator(void)
{
    pc_engine_session *s = doc_session("rgb", 8);
    CHECK(s != NULL);
    /* No paths yet: disabled. */
    CHECK(pc_engine_session_is_enabled(s, "file.export.pathsToIllustrator") == false);
    CHECK(ex(s, "file.export.pathsToIllustrator", "{}", NULL) != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    g_tmp = getenv("TMPDIR");
    if (g_tmp == NULL || g_tmp[0] == '\0') {
        g_tmp = "/tmp";
    }
    /* Block every real spool for the whole process (the transport test opts out locally). */
    setenv("PHOTOCRAFT_NO_PRINT", "1", 1);

    RUN(test_dry_run_renders_and_reports_backend);
    RUN(test_photocraft_manages_converts_to_printer_profile);
    RUN(test_printer_profile_from_icc_file);
    RUN(test_emulsion_down_mirrors_the_page);
    RUN(test_print_one_copy_repeats_last_settings);
    RUN(test_dry_run_without_output_retains_no_pdf);
    RUN(test_non_sending_requires_output_and_forgets_send);
    RUN(test_disabled_printing_never_spools);
    RUN(test_printers_lists_destinations);
    RUN(test_spool_transport_with_fake_lp);
    RUN(test_package_copies_links_and_relinks);
    RUN(test_paths_to_illustrator);
    return pc_t_finish();
}
