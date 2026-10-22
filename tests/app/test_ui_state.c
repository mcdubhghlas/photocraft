/* ui_state: save/load round-trip to the config dir, and removal of a corrupt file. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/ui_state.h"
#include "harness.h"

static char *make_tmp(void)
{
    char *base = getenv("TMPDIR");
    static char path[256];
    (void)snprintf(path, sizeof path, "%s/pc-uistate-XXXXXX", (base && base[0]) ? base : "/tmp");
    return mkdtemp(path);
}

static void test_path(void)
{
    char out[256];
    CHECK(pc_app_ui_state_path("/cfg", out, sizeof out));
    CHECK_EQ_STR(out, "/cfg/ui_state.json");
    CHECK(!pc_app_ui_state_path(NULL, out, sizeof out));
}

static void test_round_trip(void)
{
    char *dir = make_tmp();
    CHECK(dir != NULL);
    pc_ui_state s;
    pc_ui_state_init(&s);
    s.dock_width = 345.0f;
    s.palette_open = true;
    pc_error err;
    pc_error_clear(&err);
    CHECK_EQ_INT(pc_app_ui_state_save(&s, dir, &err), PC_OK);
    pc_ui_state_fini(&s);

    pc_ui_state b;
    pc_ui_state_init(&b);
    CHECK(b.dock_width != 345.0f); /* a fresh default differs */
    pc_app_ui_state_load(&b, dir);
    CHECK_NEAR(b.dock_width, 345.0, 0.01);
    CHECK(b.palette_open);
    pc_ui_state_fini(&b);
}

static void test_corrupt_file_removed(void)
{
    char *dir = make_tmp();
    CHECK(dir != NULL);
    char path[300];
    CHECK(pc_app_ui_state_path(dir, path, sizeof path));
    FILE *f = fopen(path, "wb");
    if (f) {
        (void)fputs("{ this is not valid json", f);
        (void)fclose(f);
    }
    pc_ui_state s;
    pc_ui_state_init(&s);
    pc_app_ui_state_load(&s, dir); /* must not crash; keeps defaults */
    pc_ui_state_fini(&s);
    CHECK(access(path, F_OK) != 0); /* the corrupt file is removed */
}

static void test_missing_dir_is_harmless(void)
{
    pc_ui_state s;
    pc_ui_state_init(&s);
    pc_app_ui_state_load(&s, "/nonexistent-pc-dir-xyz"); /* no crash */
    pc_app_ui_state_load(&s, NULL);
    pc_ui_state_fini(&s);
    CHECK(1);
}

int main(int argc, char **argv)
{
    RUN(test_path);
    RUN(test_round_trip);
    RUN(test_corrupt_file_removed);
    RUN(test_missing_dir_is_harmless);
    return pc_t_finish();
}
