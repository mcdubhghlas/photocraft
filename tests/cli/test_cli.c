/* photocraft-cli: in-process smoke tests of pc_cli_run (port of apps/photocraft-cli tests, basics)
 * plus usage/graceful-failure cases. */
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli/cli.h"

/* Run the CLI with argv, capturing stdout and stderr into caller buffers. Returns the exit code. */
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

static void test_help_version_unknown(void)
{
    char out[4096], err[4096];
    int code = run((char *[]){"--help"}, 1, out, sizeof out, err, sizeof err);
    CHECK_EQ_INT(code, 0);
    CHECK_CONTAINS(out, "photocraft-cli");
    code = run((char *[]){"--version"}, 1, out, sizeof out, err, sizeof err);
    CHECK_EQ_INT(code, 0);
    CHECK_CONTAINS(out, "photocraft-cli");
    code = run((char *[]){"bogus"}, 1, out, sizeof out, err, sizeof err);
    CHECK_EQ_INT(code, 2);
    CHECK_CONTAINS(err, "unknown command");
    /* An unknown flag for a known subcommand is a usage error. */
    code = run((char *[]){"commands", "--fromat", "x"}, 3, out, sizeof out, err, sizeof err);
    CHECK_EQ_INT(code, 2);
    CHECK_CONTAINS(err, "unknown flag");
}

static void test_commands(void)
{
    char out[1 << 20], err[4096];
    int code = run((char *[]){"commands", "--filter", "file.new"}, 3, out, sizeof out, err, sizeof err);
    CHECK_EQ_INT(code, 0);
    CHECK_CONTAINS(out, "file.new");
    code = run((char *[]){"commands", "--json", "--filter", "file.new"}, 4, out, sizeof out, err, sizeof err);
    CHECK_EQ_INT(code, 0);
    CHECK_CONTAINS(out, "\"id\"");
}

static void test_run_new_and_info_and_convert(void)
{
    char out[1 << 16], err[1 << 16];
    const char *tmp = getenv("TMPDIR");
    char native[512], png[512];
    snprintf(native, sizeof native, "%s/pc-cli-%d.pcraft", tmp ? tmp : "/tmp", (int)getpid());
    snprintf(png, sizeof png, "%s/pc-cli-%d.png", tmp ? tmp : "/tmp", (int)getpid());

    int code = run((char *[]){"run", "--new", "{\"width\":16,\"height\":12}", "--cmd", "layer.new.layer",
                              "--out", native},
                   7, out, sizeof out, err, sizeof err);
    CHECK_EQ_INT(code, 0);
    CHECK_CONTAINS(out, "layer.new.layer");

    code = run((char *[]){"info", native, "--compact"}, 3, out, sizeof out, err, sizeof err);
    CHECK_EQ_INT(code, 0);
    CHECK_CONTAINS(out, "\"width\":16");
    CHECK_CONTAINS(out, "\"layers\"");

    code = run((char *[]){"convert", native, png}, 3, out, sizeof out, err, sizeof err);
    CHECK_EQ_INT(code, 0);

    code = run((char *[]){"info", png, "--compact"}, 3, out, sizeof out, err, sizeof err);
    CHECK_EQ_INT(code, 0);
    CHECK_CONTAINS(out, "\"width\":16");

    (void)remove(native);
    (void)remove(png);
}

static void test_run_usage_errors(void)
{
    char out[4096], err[4096];
    /* run with neither <file> nor --new is a usage error. */
    int code = run((char *[]){"run", "--cmd", "layer.new.layer"}, 3, out, sizeof out, err, sizeof err);
    CHECK_EQ_INT(code, 1);
    CHECK_CONTAINS(err, "exactly one of");
    /* run with bad --new JSON. */
    code = run((char *[]){"run", "--new", "{not json", "--cmd", "layer.new.layer"}, 5, out, sizeof out, err,
               sizeof err);
    CHECK_EQ_INT(code, 1);
    CHECK_CONTAINS(err, "--new");
}

int main(int argc, char **argv)
{
    RUN(test_help_version_unknown);
    RUN(test_commands);
    RUN(test_run_new_and_info_and_convert);
    RUN(test_run_usage_errors);
    return pc_t_finish();
}
