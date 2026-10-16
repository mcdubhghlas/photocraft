/* Headless PhotoCraft command line (port of apps/photocraft-cli). `pc_cli_run` is the whole
 * program, so it can be tested in-process as well as through the binary. */
#ifndef PC_CLI_CLI_H
#define PC_CLI_CLI_H

#include <stdio.h>

extern const char PC_CLI_USAGE[];

/* Run the CLI with argv[0..argc) (the arguments after the program name). Returns the process exit
 * code (0 ok, 1 failure, 2 usage). Diagnostics go to `err`, normal output to `out`. */
int pc_cli_run(int argc, char **argv, FILE *out, FILE *err);

#endif /* PC_CLI_CLI_H */
