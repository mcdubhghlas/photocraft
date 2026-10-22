/* services: POSIX shell-word quoting, the XWayland relaunch command, and the Beep-When-Done bell. */
#include <stdio.h>
#include <string.h>

#include "app/services.h"
#include "harness.h"

static void test_shell_word_bare_and_quoted(void)
{
    char out[256];
    pc_error err;
    /* Safe words pass through untouched. */
    CHECK_EQ_INT(pc_services_shell_word("photocraft", out, sizeof out, &err), PC_OK);
    CHECK_EQ_STR(out, "photocraft");
    CHECK_EQ_INT(pc_services_shell_word("/usr/bin/photocraft-1.0_x", out, sizeof out, &err), PC_OK);
    CHECK_EQ_STR(out, "/usr/bin/photocraft-1.0_x");
    CHECK_EQ_INT(pc_services_shell_word("ai.storyteller.photocraft", out, sizeof out, &err), PC_OK);
    CHECK_EQ_STR(out, "ai.storyteller.photocraft");

    /* Anything with an unsafe byte is single-quoted; the empty string becomes ''. */
    CHECK_EQ_INT(pc_services_shell_word("", out, sizeof out, &err), PC_OK);
    CHECK_EQ_STR(out, "''");
    CHECK_EQ_INT(pc_services_shell_word("a b", out, sizeof out, &err), PC_OK);
    CHECK_EQ_STR(out, "'a b'");
    CHECK_EQ_INT(pc_services_shell_word("rm -rf /;echo", out, sizeof out, &err), PC_OK);
    CHECK_EQ_STR(out, "'rm -rf /;echo'");

    /* Embedded single quotes are escaped as '\''. */
    CHECK_EQ_INT(pc_services_shell_word("it's", out, sizeof out, &err), PC_OK);
    CHECK_EQ_STR(out, "'it'\\''s'");
}

static void test_shell_word_bounds(void)
{
    char small[4];
    pc_error err;
    CHECK_EQ_INT(pc_services_shell_word("photocraft", small, sizeof small, &err), PC_ERR_LIMIT);
    CHECK_EQ_STR(small, ""); /* left empty on overflow */
    CHECK_EQ_INT(pc_services_shell_word(NULL, small, sizeof small, &err), PC_ERR_INVALID);
}

static void test_xwayland_command(void)
{
    char out[256];
    bool avail = true;
    pc_error err;

    /* Flatpak: X11 socket only. */
    CHECK_EQ_INT(pc_services_xwayland_command("ai.storyteller.photocraft", NULL, false, out,
                                              sizeof out, &avail, &err),
                 PC_OK);
    CHECK(avail);
    CHECK_EQ_STR(out, "flatpak run --nosocket=wayland --socket=x11 ai.storyteller.photocraft");

    /* A Flatpak id needing quoting is quoted. */
    CHECK_EQ_INT(pc_services_xwayland_command("weird id", NULL, false, out, sizeof out, &avail, &err),
                 PC_OK);
    CHECK_CONTAINS(out, "'weird id'");

    /* AppImage relaunches by its own path when an X server is present. */
    CHECK_EQ_INT(pc_services_xwayland_command(NULL, "/opt/App.AppImage", true, out, sizeof out,
                                              &avail, &err),
                 PC_OK);
    CHECK(avail);
    CHECK_EQ_STR(out, "WAYLAND_DISPLAY= /opt/App.AppImage");

    /* Plain install: photocraft on PATH. */
    CHECK_EQ_INT(pc_services_xwayland_command(NULL, NULL, true, out, sizeof out, &avail, &err), PC_OK);
    CHECK(avail);
    CHECK_EQ_STR(out, "WAYLAND_DISPLAY= photocraft");

    /* No X server outside Flatpak: nothing to run on. */
    CHECK_EQ_INT(pc_services_xwayland_command(NULL, NULL, false, out, sizeof out, &avail, &err),
                 PC_OK);
    CHECK(!avail);
    CHECK_EQ_STR(out, "");
    CHECK_EQ_INT(pc_services_xwayland_command("", "", false, out, sizeof out, &avail, &err), PC_OK);
    CHECK(!avail);
}

static void test_beep_writes_the_bell(void)
{
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    pc_services_beep_to(f);
    rewind(f);
    int c = fgetc(f);
    CHECK_EQ_INT(c, 0x07);
    CHECK_EQ_INT(fgetc(f), EOF);
    fclose(f);
    pc_services_beep_to(NULL); /* must not crash */
}

int main(int argc, char **argv)
{
    RUN(test_shell_word_bare_and_quoted);
    RUN(test_shell_word_bounds);
    RUN(test_xwayland_command);
    RUN(test_beep_writes_the_bell);
    return pc_t_finish();
}
