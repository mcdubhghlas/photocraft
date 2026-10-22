#include "harness.h"
#include "ui/file_paths.h"
#include <string.h>

static int sign(int v)
{
    return v < 0 ? -1 : v > 0 ? 1 : 0;
}

static void test_natural_cmp(void)
{
    /* Reference contract (file_open.rs natural_cmp). */
    CHECK(sign(pc_ui_natural_cmp("File_2", "File_10")) == -1);
    CHECK(sign(pc_ui_natural_cmp("File_10", "File_2")) == 1);
    CHECK(sign(pc_ui_natural_cmp("file_1", "File_2")) == -1);  /* case-insensitive */
    CHECK(pc_ui_natural_cmp("abc", "abc") == 0);
    /* Leading zeros ignored for value, then length, then bytes; total tie-break on exact bytes. */
    CHECK(pc_ui_natural_cmp("File_03", "File_3") != 0); /* "03" vs "3" tie-break differs */
    CHECK(sign(pc_ui_natural_cmp("File_9", "File_10")) == -1);
    CHECK(sign(pc_ui_natural_cmp("File_10", "File_100")) == -1);
    /* Sort order from the reference test. */
    const char *want[] = {"A.psd",     "b.psd",     "file_1.psd", "File_03.psd",
                          "File_9.psd", "File_10.psd", "File_100.psd"};
    for (size_t i = 0; i + 1 < sizeof want / sizeof *want; i++) {
        CHECK(pc_ui_natural_cmp(want[i], want[i + 1]) < 0);
    }
    /* Totality and antisymmetry over the set. */
    for (size_t i = 0; i < sizeof want / sizeof *want; i++) {
        for (size_t j = 0; j < sizeof want / sizeof *want; j++) {
            int f = sign(pc_ui_natural_cmp(want[i], want[j]));
            int r = sign(pc_ui_natural_cmp(want[j], want[i]));
            CHECK(f == -r);
            CHECK((i == j) == (f == 0));
        }
    }
    /* NULL safety. */
    CHECK(pc_ui_natural_cmp(NULL, NULL) == 0);
    CHECK(pc_ui_natural_cmp(NULL, "a") < 0);
    CHECK(pc_ui_natural_cmp("a", NULL) > 0);
    /* Equal value, different length: "10" < "010"? zero-stripped "10"=="10" -> tie-break "10"<"010". */
    CHECK(pc_ui_natural_cmp("x10", "x010") > 0); /* '1' < '0'? bytes: "x10" vs "x010": '1'>'0' */
}

static void test_display_name(void)
{
    CHECK(!strcmp(pc_ui_display_name("/a/b/photo.psd"), "photo.psd"));
    CHECK(!strcmp(pc_ui_display_name("photo.psd"), "photo.psd"));
    CHECK(!strcmp(pc_ui_display_name(""), ""));
    CHECK(!strcmp(pc_ui_display_name("/"), "/"));
    CHECK(!strcmp(pc_ui_display_name("/a/b/"), "/a/b/")); /* empty last segment -> whole path */
    CHECK(!strcmp(pc_ui_display_name(NULL), ""));
}

static void test_lowercase_extension(void)
{
    char out[128];
    CHECK(pc_ui_lowercase_extension("/exports/chosen.JPEG", out, sizeof out));
    CHECK(!strcmp(out, "/exports/chosen.jpeg"));
    CHECK(pc_ui_lowercase_extension("X.PSD", out, sizeof out) && !strcmp(out, "X.psd"));
    /* Stem and folders keep their spelling. */
    CHECK(pc_ui_lowercase_extension("/My DIR/My Image.TIF", out, sizeof out));
    CHECK(!strcmp(out, "/My DIR/My Image.tif"));
    /* Already lowercase -> no change, copy still produced. */
    CHECK(!pc_ui_lowercase_extension("photo.png", out, sizeof out));
    CHECK(!strcmp(out, "photo.png"));
    /* No extension / hidden file -> unchanged. */
    CHECK(!pc_ui_lowercase_extension("Makefile", out, sizeof out) && !strcmp(out, "Makefile"));
    CHECK(!pc_ui_lowercase_extension(".BASHRC", out, sizeof out) && !strcmp(out, ".BASHRC"));
    /* Multi-dot: only the last extension is touched. */
    CHECK(pc_ui_lowercase_extension("a.Tar.GZ", out, sizeof out) && !strcmp(out, "a.Tar.gz"));
    /* Too-small buffer: nothing written, false. */
    char tiny[4];
    CHECK(!pc_ui_lowercase_extension("photo.PNG", tiny, sizeof tiny));
    /* NULL safety. */
    CHECK(!pc_ui_lowercase_extension(NULL, out, sizeof out));
    CHECK(!pc_ui_lowercase_extension("x.X", NULL, 0));
}

static void test_last_used_dir(void)
{
    char out[256];
    CHECK(pc_ui_last_used_dir("/pics/cat.psd", out, sizeof out) && !strcmp(out, "/pics"));
    CHECK(pc_ui_last_used_dir("/foo", out, sizeof out) && !strcmp(out, "/"));
    CHECK(!pc_ui_last_used_dir("cat.psd", out, sizeof out)); /* bare name: empty parent */
    CHECK(!pc_ui_last_used_dir("/", out, sizeof out));       /* root has no parent */
    CHECK(!pc_ui_last_used_dir("", out, sizeof out));
    CHECK(!pc_ui_last_used_dir(NULL, out, sizeof out));
    CHECK(pc_ui_last_used_dir("/a/b/c/d.png", out, sizeof out) && !strcmp(out, "/a/b/c"));
    /* Too-small buffer. */
    char tiny[3];
    CHECK(!pc_ui_last_used_dir("/pics/cat.psd", tiny, sizeof tiny));
}

static void test_is_tiff_path(void)
{
    CHECK(pc_ui_is_tiff_path("a.tif"));
    CHECK(pc_ui_is_tiff_path("dir/sub/a.TIFF"));
    CHECK(pc_ui_is_tiff_path("C:\\x\\y.tiff"));
    CHECK(pc_ui_is_tiff_path("photo.Tif"));
    CHECK(!pc_ui_is_tiff_path("a.tif.psd"));
    CHECK(!pc_ui_is_tiff_path(".tif"));
    CHECK(!pc_ui_is_tiff_path("tif"));
    CHECK(!pc_ui_is_tiff_path("a.png"));
    CHECK(!pc_ui_is_tiff_path("a.tiffx"));
    CHECK(!pc_ui_is_tiff_path(NULL));
    CHECK(!pc_ui_is_tiff_path(""));
}

int main(int argc, char **argv)
{
    RUN(test_natural_cmp);
    RUN(test_display_name);
    RUN(test_lowercase_extension);
    RUN(test_is_tiff_path);
    RUN(test_last_used_dir);
    return pc_t_finish();
}
