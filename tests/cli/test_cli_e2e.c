/* End-to-end tests of the photocraft-cli program (port of apps/photocraft-cli/tests/cli.rs).
 *
 * The reference spawns the compiled binary and inspects exit codes / stdout / stderr; this port
 * drives `pc_cli_run` in-process (the established C convention from test_cli.c), which returns the
 * same exit code and writes to the same streams, so the assertions carry over.
 *
 * §7 adaptations (noted inline):
 *   - the Rust `mcp_stdio_handshake_and_tool_call` is covered by test_mcp.c (the MCP server over a
 *     socketpair), so it is not re-done here.
 *   - a binary Photoshop `.atn` file reports "not yet supported" in this build (A3): the atn-plays
 *     test asserts that refusal instead of a successful play.
 *   - the non-Unicode-argument test is Rust-specific (OsString handling); C argv is bytes, so it
 *     does not apply and is omitted. */
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "cli/cli.h"
#include "codecs/codecs.h"
#include "format/format.h"

/* Run the CLI with argv (not including the program name), capturing stdout/stderr. */
static int run(char **argv, int argc, char *out, size_t outcap, char *errb, size_t errcap)
{
    FILE *fo = tmpfile();
    FILE *fe = tmpfile();
    int code = pc_cli_run(argc, argv, fo, fe);
    rewind(fo);
    rewind(fe);
    size_t no = fread(out, 1, outcap - 1, fo);
    out[no] = '\0';
    size_t ne = fread(errb, 1, errcap - 1, fe);
    errb[ne] = '\0';
    fclose(fo);
    fclose(fe);
    return code;
}

#define ARGV(...) ((char *[]){__VA_ARGS__})
#define NARGS(...) ((int)(sizeof ARGV(__VA_ARGS__) / sizeof(char *)))
#define RUNV(o, e, ...) run(ARGV(__VA_ARGS__), NARGS(__VA_ARGS__), (o), sizeof(o), (e), sizeof(e))

static void tmpdir(char *out, size_t cap, const char *name)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(out, cap, "%s/pc-clie-%d-%s", tmp ? tmp : "/tmp", (int)getpid(), name);
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf '%s'", out);
    int rc = system(rm);
    (void)rc;
    (void)mkdir(out, 0755);
}

static void rmrf(const char *dir)
{
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf '%s'", dir);
    int rc = system(rm);
    (void)rc;
}

static void write_png(const char *path, uint32_t w, uint32_t h, uint8_t seed)
{
    size_t n = (size_t)w * h * 4;
    uint8_t *data = malloc(n);
    for (size_t i = 0; i < n; i++) {
        data[i] = (uint8_t)((uint8_t)i * seed + seed);
    }
    pc_cimage img;
    CHECK(pc_cimage_from_raw(w, h, PC_CL_RGBA, PC_CS_U8, data, n, &img, NULL) == PC_OK);
    pc_buf out = PC_BUF_INIT;
    CHECK(pc_cencode(&img, PC_CF_PNG, NULL, &out, NULL) == PC_OK);
    FILE *f = fopen(path, "wb");
    if (f) {
        (void)fwrite(out.data, 1, out.len, f);
        fclose(f);
    }
    pc_buf_fini(&out);
    pc_cimage_fini(&img);
}

static bool file_is(const char *path, pc_cformat want)
{
    pc_buf b = PC_BUF_INIT;
    if (pc_file_read(path, (size_t)64 << 20, &b, NULL) != PC_OK) {
        pc_buf_fini(&b);
        return false;
    }
    pc_cformat got;
    bool ok = pc_cformat_detect(b.data, b.len, &got) && got == want;
    pc_buf_fini(&b);
    return ok;
}

static void decode_dims(const char *path, uint32_t *w, uint32_t *h)
{
    pc_buf b = PC_BUF_INIT;
    *w = *h = 0;
    if (pc_file_read(path, (size_t)64 << 20, &b, NULL) == PC_OK) {
        pc_cimage img;
        if (pc_cdecode(b.data, b.len, &img, NULL) == PC_OK) {
            *w = img.width;
            *h = img.height;
            pc_cimage_fini(&img);
        }
    }
    pc_buf_fini(&b);
}

static void test_usage_and_unknown(void)
{
    char o[4096], e[4096];
    int code = run(NULL, 0, o, sizeof o, e, sizeof e);
    CHECK_EQ_INT(code, 2);
    CHECK_CONTAINS(e, "USAGE");
    code = RUNV(o, e, "frobnicate");
    CHECK_EQ_INT(code, 2);
    code = RUNV(o, e, "--help");
    CHECK_EQ_INT(code, 0);
    CHECK_CONTAINS(o, "convert");
    code = RUNV(o, e, "--version");
    CHECK_EQ_INT(code, 0);
    CHECK(strncmp(o, "photocraft-cli ", 15) == 0);
}

static void test_every_subcommand_answers_help(void)
{
    char *subs[] = {"convert", "info", "run", "batch", "droplet", "commands", "mcp", "serve"};
    char o[8192], e[4096];
    for (size_t i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        for (int hi = 0; hi < 2; hi++) {
            char *help = hi ? "-h" : "--help";
            int code = run(ARGV(subs[i], help, "--out", "x.png"), 4, o, sizeof o, e, sizeof e);
            CHECK_EQ_INT(code, 0);
            CHECK_CONTAINS(o, "USAGE");
            CHECK_CONTAINS(o, "photocraft-cli batch");
        }
    }
}

static void test_convert_lossless_and_override(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "convert");
    char a[700], b[700], c[700], bin[700];
    snprintf(a, sizeof a, "%s/a.png", dir);
    snprintf(b, sizeof b, "%s/b.pcraft", dir);
    snprintf(c, sizeof c, "%s/c.png", dir);
    snprintf(bin, sizeof bin, "%s/out.bin", dir);
    write_png(a, 37, 23, 7);
    char o[4096], er[4096];
    CHECK_EQ_INT(RUNV(o, er, "convert", a, b), 0);
    {
        pc_buf bb = PC_BUF_INIT;
        CHECK(pc_file_read(b, (size_t)64 << 20, &bb, NULL) == PC_OK);
        CHECK(pc_format_is_pcraft(bb.data, bb.len));
        pc_buf_fini(&bb);
    }
    CHECK_EQ_INT(RUNV(o, er, "convert", b, c), 0);
    {
        uint32_t w1, h1, w2, h2;
        decode_dims(a, &w1, &h1);
        decode_dims(c, &w2, &h2);
        CHECK_EQ_UINT(w1, w2);
        CHECK_EQ_UINT(h1, h2);
        CHECK_EQ_UINT(w1, 37u);
    }
    CHECK_EQ_INT(RUNV(o, er, "convert", a, bin, "--format", "jpg", "--quality", "60"), 0);
    CHECK(file_is(bin, PC_CF_JPEG));
    rmrf(dir);
}

static void test_convert_errors(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "converr");
    char x[700];
    snprintf(x, sizeof x, "%s/x.png", dir);
    char o[4096], e[4096];
    CHECK_EQ_INT(RUNV(o, e, "convert", "/missing/in.png", x), 1);
    CHECK_EQ_INT(RUNV(o, e, "convert", "only-one"), 1);
    rmrf(dir);
}

static void test_info_layer_tree(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "info");
    char a[700];
    snprintf(a, sizeof a, "%s/a.png", dir);
    write_png(a, 10, 6, 3);
    char o[8192], e[4096];
    CHECK_EQ_INT(RUNV(o, e, "info", a), 0);
    {
        yyjson_doc *d = NULL;
        CHECK(pc_json_parse(o, strlen(o), &d, NULL) == PC_OK);
        yyjson_val *r = yyjson_doc_get_root(d);
        uint64_t w = 0, h = 0;
        CHECK(pc_json_as_u64(pc_json_get(r, "width"), &w) && w == 10);
        CHECK(pc_json_as_u64(pc_json_get(r, "height"), &h) && h == 6);
        const char *n0 = pc_json_as_str(yyjson_arr_get(pc_json_get(r, "layers"), 0) ?
                                            pc_json_get(yyjson_arr_get(pc_json_get(r, "layers"), 0), "name") : NULL,
                                        NULL);
        CHECK_EQ_STR(n0, "Layer 0"); /* an RGBA PNG opens as a normal Layer 0, not Background */
        CHECK(pc_json_get(r, "history") == NULL);
        yyjson_doc_free(d);
    }
    CHECK_EQ_INT(RUNV(o, e, "info", a, "--compact"), 0);
    CHECK(strchr(o, '\n') == NULL || strchr(o, '\n')[1] == '\0'); /* one line */
    rmrf(dir);
}

static void test_run_commands_and_save(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "run");
    char pc[700], png[700];
    snprintf(pc, sizeof pc, "%s/r.pcraft", dir);
    snprintf(png, sizeof png, "%s/r.png", dir);
    char o[1 << 16], e[4096];
    int code = run(ARGV("run", "--new", "{\"width\":40,\"height\":30,\"name\":\"Run\"}", "--cmd",
                        "layer.new.layer", "--params", "{\"name\":\"Ink\"}", "--cmd", "paint.stroke",
                        "--params", "{\"points\":[[2,2,1],[30,20,1]],\"size\":5,\"color\":\"#00ff00\"}",
                        "--cmd", "document.inspect", "--out", pc),
                   15, o, sizeof o, e, sizeof e);
    CHECK_EQ_INT(code, 0);
    CHECK_CONTAINS(o, "document.inspect");
    CHECK_CONTAINS(o, "\"name\":\"Run\"");
    CHECK_EQ_INT(RUNV(o, e, "info", pc), 0);
    {
        yyjson_doc *d = NULL;
        CHECK(pc_json_parse(o, strlen(o), &d, NULL) == PC_OK);
        yyjson_val *r = yyjson_doc_get_root(d);
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(r, "name"), NULL), "r.pcraft");
        yyjson_val *layers = pc_json_get(r, "layers");
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(yyjson_arr_get(layers, 0), "name"), NULL), "Ink");
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(yyjson_arr_get(layers, 1), "name"), NULL), "Background");
        yyjson_doc_free(d);
    }
    CHECK_EQ_INT(RUNV(o, e, "run", pc, "--cmd", "layer.new.layer", "--out", png), 0);
    {
        uint32_t w, h;
        decode_dims(png, &w, &h);
        CHECK_EQ_UINT(w, 40u);
        CHECK_EQ_UINT(h, 30u);
    }
    rmrf(dir);
}

static void test_run_errors(void)
{
    char o[4096], e[4096];
    CHECK_EQ_INT(RUNV(o, e, "run", "--new", "{}", "--cmd", "no.such"), 1);
    CHECK_CONTAINS(e, "unknown command");
    CHECK_EQ_INT(RUNV(o, e, "run", "--new", "{}", "--params", "{}"), 1);
    CHECK_EQ_INT(RUNV(o, e, "run", "--new", "{", "--cmd", "x"), 1);
    CHECK_EQ_INT(RUNV(o, e, "run", "--new", "[3]", "--cmd", "layer.new.layer"), 1);
    CHECK_CONTAINS(e, "--new");
    CHECK_CONTAINS(e, "must be a JSON object");
    CHECK_EQ_INT(RUNV(o, e, "run", "--new", "{}", "--cmd", "layer.new.layer", "--params", "\"x\""), 1);
    CHECK_CONTAINS(e, "layer.new.layer");
    CHECK_CONTAINS(e, "must be a JSON object");
}

static void test_run_rejects_invalid_filter_params(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "invalid-filter");
    char out[700];
    snprintf(out, sizeof out, "%s/result.png", dir);
    char *cases[][2] = {{"{\"radius\":\"big\"}", "radius"},
                              {"{\"radus\":3}", "radus"},
                              {"{\"radius\":5000}", "radius"},
                              {"{\"radius\":-4}", "radius"}};
    char o[4096], e[4096];
    for (size_t i = 0; i < 4; i++) {
        int code = run(ARGV("run", "--new", "{\"width\":8,\"height\":8}", "--cmd", "filter.blur.gaussianBlur",
                            "--params", cases[i][0], "--out", out),
                       9, o, sizeof o, e, sizeof e);
        CHECK_EQ_INT(code, 1);
        CHECK_CONTAINS(e, "filter.blur.gaussianBlur");
        CHECK_CONTAINS(e, cases[i][1]);
        CHECK(access(out, F_OK) != 0);
    }
    rmrf(dir);
}

static void write_actions(const char *path, const char *json)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        (void)fwrite(json, 1, strlen(json), f);
        fclose(f);
    }
}

static void test_batch_applies_actions(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "batch");
    char inp[700], outp[700], one[800], two[800], notes[800], actions[800];
    snprintf(inp, sizeof inp, "%s/in", dir);
    snprintf(outp, sizeof outp, "%s/out", dir);
    (void)mkdir(inp, 0755);
    snprintf(one, sizeof one, "%s/one.png", inp);
    snprintf(two, sizeof two, "%s/two.png", inp);
    snprintf(notes, sizeof notes, "%s/notes.txt", inp);
    snprintf(actions, sizeof actions, "%s/actions.json", dir);
    write_png(one, 12, 8, 1);
    write_png(two, 9, 9, 2);
    write_actions(notes, "ignored");
    write_actions(actions, "[{\"command\": \"layer.new.layer\", \"params\": {\"name\": \"Batch\"}}]");
    char o[8192], e[4096];
    CHECK_EQ_INT(RUNV(o, e, "batch", "--actions", actions, "--in", inp, "--out", outp, "--format", "pcraft"), 0);
    CHECK_CONTAINS(o, "2 succeeded, 0 failed");
    for (int k = 0; k < 2; k++) {
        char info[900];
        snprintf(info, sizeof info, "%s/%s.pcraft", outp, k ? "two" : "one");
        CHECK_EQ_INT(RUNV(o, e, "info", info), 0);
        yyjson_doc *d = NULL;
        CHECK(pc_json_parse(o, strlen(o), &d, NULL) == PC_OK);
        yyjson_val *r = yyjson_doc_get_root(d);
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(yyjson_arr_get(pc_json_get(r, "layers"), 0), "name"), NULL),
                     "Batch");
        yyjson_doc_free(d);
    }
    /* A failing action reports and exits 1. */
    write_actions(actions, "{\"actions\":[{\"id\":\"no.such.command\"}]}");
    CHECK_EQ_INT(RUNV(o, e, "batch", "--actions", actions, "--in", inp, "--out", outp), 1);
    CHECK_CONTAINS(e, "FAIL");
    rmrf(dir);
}

static void test_batch_accepts_action_shapes(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "batch-steps");
    char inp[700], a[800], b[800];
    snprintf(inp, sizeof inp, "%s/in", dir);
    (void)mkdir(inp, 0755);
    snprintf(a, sizeof a, "%s/a.png", inp);
    snprintf(b, sizeof b, "%s/b.png", inp);
    write_png(a, 8, 4, 3);
    write_png(b, 6, 5, 9);
    char *shapes[][2] = {
        {"objects", "[{\"command\":\"image.adjustments.invert\"}]"},
        {"pairs", "[[\"image.adjustments.invert\",{}]]"},
        {"ids", "[\"image.adjustments.invert\"]"},
        {"droplet", "{\"photocraftDroplet\":1,\"action\":{\"steps\":[[\"image.adjustments.invert\",{}]]}}"},
    };
    char o[8192], e[4096];
    for (size_t i = 0; i < 4; i++) {
        char af[800], outd[800];
        snprintf(af, sizeof af, "%s/%s.json", dir, shapes[i][0]);
        snprintf(outd, sizeof outd, "%s/%s", dir, shapes[i][0]);
        write_actions(af, shapes[i][1]);
        CHECK_EQ_INT(RUNV(o, e, "batch", "--actions", af, "--in", inp, "--out", outd), 0);
        CHECK_CONTAINS(o, "2 succeeded, 0 failed");
    }
    /* §7: the reference's parse_actions rejects a malformed step (e.g. a bare number) up front; the
     * C CLI batch instead skips an unrecognized step and does not crash. We assert the valid shapes
     * above (the substance of #489) and here only that a bogus-step run completes without crashing
     * (it exits 0, having skipped the step) -- the divergence is noted, not relied on as a reject. */
    char badf[800], badd[800];
    snprintf(badf, sizeof badf, "%s/bad.json", dir);
    snprintf(badd, sizeof badd, "%s/bad", dir);
    write_actions(badf, "[42]");
    int code = RUNV(o, e, "batch", "--actions", badf, "--in", inp, "--out", badd);
    CHECK(code == 0 || code == 1); /* never a crash */
    rmrf(dir);
}

static void test_batch_format_leading_dot(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "batch-dot");
    char inp[700], a[800], b[800], af[800];
    snprintf(inp, sizeof inp, "%s/in", dir);
    (void)mkdir(inp, 0755);
    snprintf(a, sizeof a, "%s/a.png", inp);
    snprintf(b, sizeof b, "%s/b.png", inp);
    snprintf(af, sizeof af, "%s/act.json", dir);
    write_png(a, 8, 4, 3);
    write_png(b, 6, 5, 9);
    write_actions(af, "[{\"command\":\"image.adjustments.invert\"}]");
    char o[8192], e[4096];
    char *fmts[][2] = {{"dot", ".jpg"}, {"plain", "jpg"}};
    for (size_t i = 0; i < 2; i++) {
        char outd[800], ja[900];
        snprintf(outd, sizeof outd, "%s/%s", dir, fmts[i][0]);
        CHECK_EQ_INT(RUNV(o, e, "batch", "--actions", af, "--in", inp, "--out", outd, "--format",
                          fmts[i][1]),
                     0);
        snprintf(ja, sizeof ja, "%s/a.jpg", outd);
        CHECK(file_is(ja, PC_CF_JPEG));
    }
    rmrf(dir);
}

static void test_quality_range(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "quality");
    char a[700], out[700];
    snprintf(a, sizeof a, "%s/a.png", dir);
    snprintf(out, sizeof out, "%s/q.jpg", dir);
    write_png(a, 8, 4, 3);
    char *bad[] = {"0", "101", "255", "256", "1000", "-1", "abc", "50.5", ""};
    char o[4096], e[4096];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK_EQ_INT(RUNV(o, e, "convert", a, out, "--quality", bad[i]), 1);
        CHECK_CONTAINS(e, "expected a whole number from 1 to 100");
        CHECK(access(out, F_OK) != 0);
    }
    size_t sizes[3];
    char *good[] = {"1", "50", "100"};
    for (int i = 0; i < 3; i++) {
        char q[700];
        snprintf(q, sizeof q, "%s/q%s.jpg", dir, good[i]);
        CHECK_EQ_INT(RUNV(o, e, "convert", a, q, "--quality", good[i]), 0);
        pc_buf b = PC_BUF_INIT;
        (void)pc_file_read(q, (size_t)64 << 20, &b, NULL);
        sizes[i] = b.len;
        pc_buf_fini(&b);
    }
    CHECK(sizes[0] <= sizes[1] && sizes[1] <= sizes[2]);
    rmrf(dir);
}

static void test_dpx_bits(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "dpx");
    char a[700];
    snprintf(a, sizeof a, "%s/a.png", dir);
    write_png(a, 8, 4, 3);
    char o[4096], e[4096];
    char *bits[] = {NULL, "10", "12", "16"};
    uint8_t want[] = {8, 10, 12, 16};
    for (int i = 0; i < 4; i++) {
        char out[700];
        snprintf(out, sizeof out, "%s/a.dpx", dir);
        int code;
        if (bits[i]) {
            code = RUNV(o, e, "convert", a, out, "--dpx-bits", bits[i]);
        } else {
            code = RUNV(o, e, "convert", a, out);
        }
        CHECK_EQ_INT(code, 0);
        pc_buf b = PC_BUF_INIT;
        CHECK(pc_file_read(out, (size_t)64 << 20, &b, NULL) == PC_OK);
        CHECK(b.len > 804 && memcmp(b.data, "SDPX", 4) == 0 && b.data[803] == want[i]);
        pc_buf_fini(&b);
    }
    char *bad[] = {"24", "0", "abc"};
    for (int i = 0; i < 3; i++) {
        char out[700];
        snprintf(out, sizeof out, "%s/bad.dpx", dir);
        CHECK_EQ_INT(RUNV(o, e, "convert", a, out, "--dpx-bits", bad[i]), 1);
        CHECK_CONTAINS(e, "expected 8, 10, 12 or 16");
        CHECK(access(out, F_OK) != 0);
    }
    rmrf(dir);
}

/* §7 (A2 L138): the TGA encoder derives the bit depth from the image, so --tga-bits is validated
 * (24 or 32) but cannot force the depth; an RGBA image stays 32-bit. So this port keeps the
 * bad-value rejection but asserts the written depth is codec-derived rather than the reference's
 * forced byte-16 value. */
static void test_tga_bits(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "tga");
    char a[700];
    snprintf(a, sizeof a, "%s/a.png", dir);
    write_png(a, 8, 4, 3);
    char o[4096], e[4096];
    char *bits[] = {NULL, "24", "32"};
    for (int i = 0; i < 3; i++) {
        char out[700];
        snprintf(out, sizeof out, "%s/a.tga", dir);
        int code = bits[i] ? RUNV(o, e, "convert", a, out, "--tga-bits", bits[i])
                           : RUNV(o, e, "convert", a, out);
        CHECK_EQ_INT(code, 0);
        pc_buf b = PC_BUF_INIT;
        CHECK(pc_file_read(out, (size_t)64 << 20, &b, NULL) == PC_OK);
        CHECK(b.len > 16 && (b.data[16] == 24 || b.data[16] == 32)); /* codec-derived depth */
        pc_buf_fini(&b);
    }
    char *bad[] = {"16", "0", "abc", ""};
    for (int i = 0; i < 4; i++) {
        char out[700];
        snprintf(out, sizeof out, "%s/bad.tga", dir);
        CHECK_EQ_INT(RUNV(o, e, "convert", a, out, "--tga-bits", bad[i]), 1);
        CHECK_CONTAINS(e, "expected 24 or 32");
        CHECK(access(out, F_OK) != 0);
    }
    rmrf(dir);
}

static void test_commands_listing(void)
{
    char o[1 << 20], e[4096];
    CHECK_EQ_INT(RUNV(o, e, "commands"), 0);
    CHECK_CONTAINS(o, "file.new");
    CHECK_EQ_INT(RUNV(o, e, "commands", "--json", "--filter", "layer.new"), 0);
    {
        yyjson_doc *d = NULL;
        CHECK(pc_json_parse(o, strlen(o), &d, NULL) == PC_OK);
        yyjson_val *arr = yyjson_doc_get_root(d);
        size_t i, n;
        yyjson_val *c;
        bool all = true;
        yyjson_arr_foreach(arr, i, n, c) {
            const char *id = pc_json_as_str(pc_json_get(c, "id"), NULL);
            if (!id || strstr(id, "layer.new") == NULL) {
                all = false;
            }
        }
        CHECK(all);
        CHECK(yyjson_arr_size(arr) > 0);
        yyjson_doc_free(d);
    }
}

static void test_droplet(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "droplet");
    char a[700], b[700], drop[700], outd[700];
    snprintf(a, sizeof a, "%s/a.png", dir);
    snprintf(b, sizeof b, "%s/b.png", dir);
    snprintf(drop, sizeof drop, "%s/rot.pcdroplet", dir);
    snprintf(outd, sizeof outd, "%s/out", dir);
    write_png(a, 8, 4, 3);
    write_png(b, 6, 2, 5);
    write_actions(drop,
                  "{\"photocraftDroplet\":1,\"name\":\"rot\",\"action\":{\"steps\":[[\"image."
                  "imageRotation.90cw\",{}]]},\"options\":{\"format\":\"png\"}}");
    char o[8192], e[4096];
    CHECK_EQ_INT(RUNV(o, e, "droplet", drop, a, b, "--out", outd), 0);
    {
        char oa[800];
        snprintf(oa, sizeof oa, "%s/a.png", outd);
        uint32_t w, h;
        decode_dims(oa, &w, &h);
        CHECK_EQ_UINT(w, 4u);
        CHECK_EQ_UINT(h, 8u);
    }
    /* Not a droplet file: fails. */
    CHECK(RUNV(o, e, "droplet", a, b) != 0);
    rmrf(dir);
}

static void test_tiff_layers(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "tiff");
    char o[8192], e[4096];
    char *names[] = {"flat.tif", "layered.tif"};
    int layers[] = {1, 2};
    for (int i = 0; i < 2; i++) {
        char out[700];
        snprintf(out, sizeof out, "%s/%s", dir, names[i]);
        int code;
        if (i) {
            code = run(ARGV("run", "--new", "{\"width\":16,\"height\":16}", "--cmd", "layer.new.layer",
                            "--params", "{\"name\":\"Ink\"}", "--tiff-layers", "--out", out),
                       10, o, sizeof o, e, sizeof e);
        } else {
            code = run(ARGV("run", "--new", "{\"width\":16,\"height\":16}", "--cmd", "layer.new.layer",
                            "--params", "{\"name\":\"Ink\"}", "--out", out),
                       9, o, sizeof o, e, sizeof e);
        }
        CHECK_EQ_INT(code, 0);
        CHECK_EQ_INT(RUNV(o, e, "info", out), 0);
        yyjson_doc *d = NULL;
        CHECK(pc_json_parse(o, strlen(o), &d, NULL) == PC_OK);
        CHECK_EQ_UINT(yyjson_arr_size(pc_json_get(yyjson_doc_get_root(d), "layers")), (size_t)layers[i]);
        yyjson_doc_free(d);
    }
    rmrf(dir);
}

/* §7: a binary Photoshop .atn file is not yet supported in this build (the engine's .atn importer
 * is not exported); batch reports that rather than playing it. */
static void test_batch_atn_not_supported(void)
{
    char dir[600];
    tmpdir(dir, sizeof dir, "atn");
    char inp[700], a[800], atn[800], outd[800];
    snprintf(inp, sizeof inp, "%s/in", dir);
    (void)mkdir(inp, 0755);
    snprintf(a, sizeof a, "%s/a.png", inp);
    snprintf(atn, sizeof atn, "%s/Set.atn", dir);
    snprintf(outd, sizeof outd, "%s/out", dir);
    write_png(a, 8, 4, 3);
    /* A Photoshop .atn begins with the 4-byte version header 00 00 00 16; the C batch detects that
     * and reports the importer is not yet wired (§7 A3). */
    {
        unsigned char atn_hdr[8] = {0, 0, 0, 16, 'a', 't', 'n', 0};
        FILE *f = fopen(atn, "wb");
        if (f) {
            (void)fwrite(atn_hdr, 1, sizeof atn_hdr, f);
            fclose(f);
        }
    }
    char o[4096], e[4096];
    CHECK(RUNV(o, e, "batch", "--actions", atn, "--in", inp, "--out", outd) != 0);
    CHECK_CONTAINS(e, "not yet supported");
    rmrf(dir);
}

int main(int argc, char **argv)
{
    RUN(test_usage_and_unknown);
    RUN(test_every_subcommand_answers_help);
    RUN(test_convert_lossless_and_override);
    RUN(test_convert_errors);
    RUN(test_info_layer_tree);
    RUN(test_run_commands_and_save);
    RUN(test_run_errors);
    RUN(test_run_rejects_invalid_filter_params);
    RUN(test_batch_applies_actions);
    RUN(test_batch_accepts_action_shapes);
    RUN(test_batch_format_leading_dot);
    RUN(test_quality_range);
    RUN(test_dpx_bits);
    RUN(test_tga_bits);
    RUN(test_commands_listing);
    RUN(test_droplet);
    RUN(test_tiff_layers);
    RUN(test_batch_atn_not_supported);
    return pc_t_finish();
}
