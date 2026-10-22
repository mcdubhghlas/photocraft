/* photocraft-parity: write (or print) the Photoshop menu-parity report (docs/parity-checklist.md)
 * from the C menu catalog and the live command registry (REWRITE_PLAN.md Z1, driven by
 * tools/parity.sh). Headless, no GUI: it links the library and calls pc_ui_parity_generate.
 *
 *   photocraft-parity               print the report to stdout
 *   photocraft-parity --write PATH  write it to PATH (atomically)
 *
 * Lives in src/ui (layer L6) because the parity data (menu catalog + is_live) is ui-owned; the
 * headless CLI must not depend on ui, so this is its own small program. */
#include <stdio.h>
#include <string.h>

#include "base/status.h"
#include "ui/parity.h"

int main(int argc, char **argv)
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--write") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fputs("usage: photocraft-parity [--write PATH]\n", stdout);
            return 0;
        } else {
            fprintf(stderr, "photocraft-parity: unexpected argument `%s`\n", argv[i]);
            return 2;
        }
    }
    size_t live = 0, total = 0;
    pc_error e;
    pc_error_clear(&e);
    if (pc_ui_parity_generate(path, stdout, &live, &total, &e) != PC_OK) {
        fprintf(stderr, "photocraft-parity: %s\n", e.msg);
        return 1;
    }
    if (path != NULL) {
        double pct = total != 0 ? 100.0 * (double)live / (double)total : 0.0;
        fprintf(stderr, "wrote %s: %zu of %zu menu items live (%.1f%%)\n", path, live, total, pct);
    }
    return 0;
}
