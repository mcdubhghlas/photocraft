/* Keyboard shortcut parse/format/match, default and effective bindings, and the binding table.
 * Ports of the pure shortcuts.rs tests and shortcut_dispatch::bindings invariants. */
#include "harness.h"

#include "engine/engine.h"
#include "ui/shortcut_dispatch.h"
#include "ui/shortcuts.h"

#include <stdlib.h>
#include <string.h>

static void test_parse_registry(void)
{
    pc_ui_shortcut sc;
    CHECK(pc_ui_shortcut_parse("Cmd+Shift+N", &sc));
    CHECK(sc.key == PC_KEY_N);
    CHECK((sc.mods & PC_UI_MOD_COMMAND) && (sc.mods & PC_UI_MOD_SHIFT));
    CHECK(pc_ui_shortcut_parse("Cmd+]", &sc) && sc.key == PC_KEY_CLOSEBRACKET);
    CHECK(pc_ui_shortcut_parse("Cmd+=", &sc) && sc.key == PC_KEY_EQUALS);
    CHECK(pc_ui_shortcut_parse("F7", &sc) && sc.key == PC_KEY_F7);
    CHECK(!pc_ui_shortcut_parse("Cmd+Nonsense", &sc));
}

static void test_malformed(void)
{
    pc_ui_shortcut sc;
    const char *bad[] = {"Cmd+Typo+Z", "Cmd+X+Z", "Cmd+Z+X", "Cmd+Z+",
                         "Cmd+Cmd+Z", "Ctrl+Ctrl+Z", "Shift+Shift+Z", "Cmd+Shift+++"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!pc_ui_shortcut_parse(bad[i], &sc));
    }
    const char *plus[] = {"+", "Cmd+", "Cmd++", "Cmd+Shift++"};
    for (size_t i = 0; i < sizeof plus / sizeof plus[0]; i++) {
        CHECK(pc_ui_shortcut_parse(plus[i], &sc) && sc.key == PC_KEY_PLUS);
    }
    CHECK(pc_ui_shortcut_parse("Cmd+Shift+Z", &sc));
    CHECK(pc_ui_shortcut_parse("Cmd+;", &sc) && sc.key == PC_KEY_SEMICOLON);
}

static pc_ui_shortcut parse_ok(const char *s)
{
    pc_ui_shortcut sc = {0, PC_KEY_NONE};
    CHECK(pc_ui_shortcut_parse(s, &sc));
    return sc;
}

static void test_key_matches(void)
{
    unsigned cmd = PC_UI_MOD_COMMAND;
    pc_ui_shortcut zoom_in = parse_ok("Cmd+=");
    CHECK(pc_ui_shortcut_key_matches(&zoom_in, PC_KEY_EQUALS, cmd));
    CHECK(pc_ui_shortcut_key_matches(&zoom_in, PC_KEY_PLUS, cmd | PC_UI_MOD_SHIFT));
    CHECK(pc_ui_shortcut_key_matches(&zoom_in, PC_KEY_PLUS, cmd));
    CHECK(!pc_ui_shortcut_key_matches(&zoom_in, PC_KEY_PLUS, PC_UI_MOD_NONE));
    CHECK(!pc_ui_shortcut_key_matches(&zoom_in, PC_KEY_EQUALS, cmd | PC_UI_MOD_ALT));
    pc_ui_shortcut zo = parse_ok("Cmd+-");
    CHECK(pc_ui_shortcut_key_matches(&zo, PC_KEY_MINUS, cmd));
    pc_ui_shortcut z0 = parse_ok("Cmd+0");
    CHECK(pc_ui_shortcut_key_matches(&z0, PC_KEY_NUM0, cmd));
    /* Off the Mac, Ctrl held is the same primary modifier as Cmd (both bits set). */
    CHECK(pc_ui_shortcut_key_matches(&zoom_in, PC_KEY_PLUS, PC_UI_MOD_CTRL | PC_UI_MOD_COMMAND));
    pc_ui_shortcut invert = parse_ok("Cmd+I");
    CHECK(pc_ui_shortcut_key_matches(&invert, PC_KEY_I, cmd));
    CHECK(!pc_ui_shortcut_key_matches(&invert, PC_KEY_I, cmd | PC_UI_MOD_ALT));
    pc_ui_shortcut undo = parse_ok("Cmd+Z");
    CHECK(!pc_ui_shortcut_key_matches(&undo, PC_KEY_Z, cmd | PC_UI_MOD_SHIFT));
    /* ⇧ shortcuts on punctuation arrive as the shifted character. */
    pc_ui_shortcut sc_semi = parse_ok("Cmd+Shift+;");
    CHECK(pc_ui_shortcut_key_matches(&sc_semi, PC_KEY_COLON, cmd | PC_UI_MOD_SHIFT));
    pc_ui_shortcut sc_br = parse_ok("Cmd+Shift+]");
    CHECK(pc_ui_shortcut_key_matches(&sc_br, PC_KEY_CLOSECURLYBRACKET, cmd | PC_UI_MOD_SHIFT));
    pc_ui_shortcut semi = parse_ok("Cmd+;");
    CHECK(!pc_ui_shortcut_key_matches(&semi, PC_KEY_COLON, cmd | PC_UI_MOD_SHIFT));
    /* Delete and Backspace both clear. */
    pc_ui_shortcut clear = parse_ok("Delete");
    CHECK(pc_ui_shortcut_key_matches(&clear, PC_KEY_DELETE, PC_UI_MOD_NONE));
    CHECK(pc_ui_shortcut_key_matches(&clear, PC_KEY_BACKSPACE, PC_UI_MOD_NONE));
    CHECK(!pc_ui_shortcut_key_matches(&clear, PC_KEY_BACKSPACE, cmd));
}

static void test_pretty(void)
{
    char b[64];
    /* macOS notation */
    pc_ui_shortcut_pretty_ex("Cmd+Shift+N", true, b, sizeof b);
    CHECK_EQ_STR(b, "\xE2\x8C\x98\xE2\x87\xA7N"); /* ⌘⇧N */
    pc_ui_shortcut_pretty_ex("Alt", true, b, sizeof b);
    CHECK_EQ_STR(b, "\xE2\x8C\xA5"); /* ⌥ */
    pc_ui_shortcut_pretty_ex("Shift+Alt", true, b, sizeof b);
    CHECK_EQ_STR(b, "\xE2\x87\xA7\xE2\x8C\xA5"); /* ⇧⌥ */
    pc_ui_shortcut_pretty_ex("Enter", true, b, sizeof b);
    CHECK_EQ_STR(b, "\xE2\x86\xA9"); /* ↩ */
    pc_ui_shortcut_pretty_ex("Cmd+Enter", true, b, sizeof b);
    CHECK_EQ_STR(b, "\xE2\x8C\x98\xE2\x86\xA9"); /* ⌘↩ */
    /* non-macOS notation */
    pc_ui_shortcut_pretty_ex("Cmd+Shift+N", false, b, sizeof b);
    CHECK_EQ_STR(b, "Ctrl+Shift+N");
    pc_ui_shortcut_pretty_ex("Alt", false, b, sizeof b);
    CHECK_EQ_STR(b, "Alt");
    pc_ui_shortcut_pretty_ex("Shift+Alt", false, b, sizeof b);
    CHECK_EQ_STR(b, "Shift+Alt");
    pc_ui_shortcut_pretty_ex("Cmd+Enter", false, b, sizeof b);
    CHECK_EQ_STR(b, "Ctrl+Enter");
    /* The Plus key prints as "+". */
    pc_ui_shortcut_pretty_ex("Cmd+", false, b, sizeof b);
    CHECK_EQ_STR(b, "Ctrl++");
    pc_ui_shortcut_pretty_ex("Cmd+", true, b, sizeof b);
    CHECK_EQ_STR(b, "\xE2\x8C\x98+"); /* ⌘+ */
}

static void test_default_shortcut(void)
{
    char b[64];
    CHECK(pc_ui_default_shortcut("file.new", b, sizeof b) && strcmp(b, "Cmd+N") == 0);
    /* A UI command's own shortcut wins. */
    CHECK(pc_ui_default_shortcut("view.zoomIn", b, sizeof b) && strcmp(b, "Cmd+=") == 0);
    /* A toolbar tool's single-key default. */
    CHECK(pc_ui_default_shortcut("tools.select.brush", b, sizeof b) && strcmp(b, "B") == 0);
    /* A temporary tool default. */
    CHECK(pc_ui_default_shortcut("tools.temporary.hand", b, sizeof b) && strcmp(b, "Space") == 0);
    /* No binding. */
    CHECK(!pc_ui_default_shortcut("file.totallyBogus", b, sizeof b));
}

static void test_effective_shortcut(void)
{
    char b[64];
    /* Without a session, the default. */
    CHECK(pc_ui_effective_shortcut(NULL, "file.new", b, sizeof b) && strcmp(b, "Cmd+N") == 0);

    pc_engine_session *s = NULL;
    pc_error e = {0};
    CHECK(pc_engine_session_new(&s, &e) == PC_OK);
    CHECK(pc_ui_effective_shortcut(s, "file.new", b, sizeof b) && strcmp(b, "Cmd+N") == 0);
    /* A user override is picked up. */
    pc_json_value o;
    CHECK(pc_engine_session_execute_json(
              s, "prefs.set", "{\"path\":\"shortcuts.file.new\",\"value\":\"Cmd+Shift+N\"}", &o, &e) ==
          PC_OK);
    pc_json_value_fini(&o);
    CHECK(pc_ui_effective_shortcut(s, "file.new", b, sizeof b) && strcmp(b, "Cmd+Shift+N") == 0);
    /* A removed binding hides the shortcut entirely. */
    CHECK(pc_engine_session_execute_json(
              s, "prefs.set", "{\"path\":\"shortcuts.file.new\",\"value\":\"\"}", &o, &e) == PC_OK);
    pc_json_value_fini(&o);
    CHECK(!pc_ui_effective_shortcut(s, "file.new", b, sizeof b));
    pc_engine_session_free(s);
}

static void test_nav(void)
{
    CHECK(pc_ui_is_nav_command("view.zoomIn"));
    CHECK(pc_ui_is_nav_command("view.actualPixels"));
    CHECK(!pc_ui_is_nav_command("file.new"));
    CHECK(!pc_ui_is_nav_command(NULL));
}

static void test_bindings(void)
{
    pc_ui_binding *b = NULL;
    size_t n = 0;
    pc_error e = {0};
    CHECK(pc_ui_bindings(NULL, &b, &n, &e) == PC_OK);
    CHECK(n > 50);
    /* Each chord is owned by exactly one command. */
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            CHECK(!(b[i].sc.mods == b[j].sc.mods && b[i].sc.key == b[j].sc.key));
        }
    }
    /* Ordered most-modifiers-first (Shift+Alt+Command count, non-increasing). */
    int prev = 4;
    for (size_t i = 0; i < n; i++) {
        int c = 0;
        if (b[i].sc.mods & PC_UI_MOD_SHIFT) c++;
        if (b[i].sc.mods & PC_UI_MOD_ALT) c++;
        if (b[i].sc.mods & PC_UI_MOD_COMMAND) c++;
        CHECK(c <= prev);
        prev = c;
    }
    /* file.new → Cmd+N is present. */
    bool found_new = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(b[i].id, "file.new") == 0 && b[i].sc.key == PC_KEY_N &&
            b[i].sc.mods == PC_UI_MOD_COMMAND) {
            found_new = true;
        }
    }
    CHECK(found_new);
    /* The secondary Edit › Fill binding (Shift+Backspace) is present. */
    bool found_fill = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(b[i].id, "edit.fill") == 0 && b[i].sc.key == PC_KEY_BACKSPACE &&
            b[i].sc.mods == PC_UI_MOD_SHIFT) {
            found_fill = true;
        }
    }
    CHECK(found_fill);
    free(b);
}

int main(int argc, char **argv)
{
    RUN(test_parse_registry);
    RUN(test_malformed);
    RUN(test_key_matches);
    RUN(test_pretty);
    RUN(test_default_shortcut);
    RUN(test_effective_shortcut);
    RUN(test_nav);
    RUN(test_bindings);
    return pc_t_finish();
}
