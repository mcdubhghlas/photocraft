/* photocraft-cli entry point (port of apps/photocraft-cli/src/main.rs). */
#include <stdio.h>

#include "cli/cli.h"

int main(int argc, char **argv)
{
    return pc_cli_run(argc - 1, argv + 1, stdout, stderr);
}
