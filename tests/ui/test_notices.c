/* Notices (port of crates/ui-egui/src/notices.rs logic tests: auto-hide timer, cap, io warnings,
 * slow-refresh explanation, dismiss). The egui rendering and the Wayland-drop guidance (needs the
 * Services struct) are not ported here (§7). */
#include "harness.h"

#include <string.h>

#include "base/buf.h"
#include "ui/i18n.h"
#include "ui/notices.h"

static uint64_t post(pc_ui_notices *ns, const char *title)
{
    return pc_ui_notices_post(ns, title, NULL, 0, false, NULL);
}

static void test_auto_hide_after_delay(void)
{
    pc_ui_notices ns;
    pc_ui_notices_init(&ns);
    post(&ns, "Transient");
    bool has;
    double next = pc_ui_notices_expire(&ns, 100.0, false, false, true, 6.0, &has);
    CHECK(has && next == 6.0);
    CHECK_EQ_UINT(ns.n, 1);
    next = pc_ui_notices_expire(&ns, 105.0, false, false, true, 6.0, &has);
    CHECK(has && next == 1.0);
    CHECK_EQ_UINT(ns.n, 1);
    (void)pc_ui_notices_expire(&ns, 106.0, false, false, true, 6.0, &has);
    CHECK(!has);
    CHECK_EQ_UINT(ns.n, 0);
    pc_ui_notices_fini(&ns);
}

static void test_hover_pauses_and_leaving_resets(void)
{
    pc_ui_notices ns;
    pc_ui_notices_init(&ns);
    post(&ns, "Transient");
    bool has;
    const double times[] = {5.0, 6.0, 7.0, 100.0};
    for (size_t i = 0; i < 4; i++) {
        (void)pc_ui_notices_expire(&ns, times[i], true, false, true, 6.0, &has);
        CHECK(!has);
        CHECK_EQ_UINT(ns.n, 1);
    }
    /* Leaving a long hover gives a fresh delay, not an immediate expiry. */
    (void)pc_ui_notices_expire(&ns, 0.0, true, false, true, 6.0, &has);
    double next = pc_ui_notices_expire(&ns, 10.0, false, true, true, 6.0, &has);
    CHECK(has && next == 6.0);
    CHECK_EQ_UINT(ns.n, 1);
    pc_ui_notices_fini(&ns);
}

static void test_autohide_off(void)
{
    pc_ui_notices ns;
    pc_ui_notices_init(&ns);
    post(&ns, "Persistent");
    bool has;
    (void)pc_ui_notices_expire(&ns, 0.0, false, false, false, 6.0, &has);
    CHECK(!has);
    (void)pc_ui_notices_expire(&ns, 1e6, false, false, false, 6.0, &has);
    CHECK(!has);
    CHECK_EQ_UINT(ns.n, 1);
    pc_ui_notices_fini(&ns);
}

static void test_cap_keeps_a_dismissible(void)
{
    pc_ui_notices ns;
    pc_ui_notices_init(&ns);
    pc_ui_notices_post(&ns, "Keep me", NULL, 0, false, "ui.keepMe");
    for (int i = 0; i < PC_UI_MAX_NOTICES; i++) {
        post(&ns, "Transient");
    }
    CHECK_EQ_UINT(ns.n, PC_UI_MAX_NOTICES);
    bool found = false;
    for (size_t i = 0; i < ns.n; i++) {
        if (ns.items[i].dismiss_pref != NULL) {
            found = true;
        }
    }
    CHECK(found);
    pc_ui_notices_fini(&ns);
}

static void test_dismiss_returns_pref(void)
{
    pc_ui_notices ns;
    pc_ui_notices_init(&ns);
    pc_ui_notices_post(&ns, "Dismissible", NULL, 0, false, "ui.testNoticeDismissed");
    uint64_t id = ns.items[0].id;
    char pref[128];
    CHECK(pc_ui_notices_dismiss(&ns, id, pref, sizeof pref));
    CHECK_EQ_STR(pref, "ui.testNoticeDismissed");
    CHECK_EQ_UINT(ns.n, 0);
    pc_ui_notices_fini(&ns);
}

static void test_slow_cpu_refresh_once_per_document(void)
{
    pc_ui_notices ns;
    pc_ui_notices_init(&ns);
    CHECK(!pc_ui_notices_slow_cpu_refresh(&ns, 1, 400.0, PC_UI_GPU_OVER_BUDGET));
    CHECK(!pc_ui_notices_slow_cpu_refresh(&ns, 1, 0.0 / 0.0, PC_UI_GPU_OVER_BUDGET));
    CHECK_EQ_UINT(ns.n, 0);

    CHECK(pc_ui_notices_slow_cpu_refresh(&ns, 1, 55000.0, PC_UI_GPU_OVER_BUDGET));
    CHECK_EQ_UINT(ns.n, 1);
    /* lines joined contain the key phrases (English, default language). */
    pc_buf joined = PC_BUF_INIT;
    for (size_t i = 0; i < ns.items[0].n_lines; i++) {
        pc_buf_append(&joined, ns.items[0].lines[i], strlen(ns.items[0].lines[i]), NULL);
        pc_buf_append(&joined, " ", 1, NULL);
    }
    CHECK_CONTAINS(pc_buf_cstr(&joined), "55.0 s");
    CHECK_CONTAINS(pc_buf_cstr(&joined), "blend mode, opacity or visibility");
    CHECK_CONTAINS(pc_buf_cstr(&joined), "Memory usage in Preferences");
    CHECK(!ns.items[0].error);
    pc_buf_fini(&joined);

    CHECK(!pc_ui_notices_slow_cpu_refresh(&ns, 1, 55000.0, PC_UI_GPU_OVER_BUDGET));
    CHECK_EQ_UINT(ns.n, 1);

    CHECK(pc_ui_notices_slow_cpu_refresh(&ns, 2, 3000.0, "Blend If on `Overlay` (composited on the CPU)"));
    CHECK_EQ_UINT(ns.n, 2);
    pc_buf j2 = PC_BUF_INIT;
    for (size_t i = 0; i < ns.items[1].n_lines; i++) {
        pc_buf_append(&j2, ns.items[1].lines[i], strlen(ns.items[1].lines[i]), NULL);
        pc_buf_append(&j2, " ", 1, NULL);
    }
    CHECK_CONTAINS(pc_buf_cstr(&j2), "Blend If on `Overlay`");
    CHECK(strstr(pc_buf_cstr(&j2), "Memory usage") == NULL);
    pc_buf_fini(&j2);
    pc_ui_notices_fini(&ns);
}

static void test_io_warnings_status(void)
{
    pc_ui_notices ns;
    pc_ui_notices_init(&ns);
    const char *warnings[] = {"first warning", "second warning", "third warning"};
    pc_buf status = PC_BUF_INIT;
    bool err = false;
    pc_ui_notices_io_warnings(&ns, "Opened x.psd", warnings, 3, &status, &err);
    CHECK_EQ_STR(pc_buf_cstr(&status), "Opened x.psd with 3 warnings: first warning \xe2\x80\xa6");
    CHECK(err);
    CHECK_EQ_UINT(ns.items[ns.n - 1].n_lines, 3);
    /* One warning: "{what}: {first}". */
    pc_ui_notices_io_warnings(&ns, "Opened x.psd", warnings, 1, &status, &err);
    CHECK_EQ_STR(pc_buf_cstr(&status), "Opened x.psd: first warning");
    pc_buf_fini(&status);
    pc_ui_notices_fini(&ns);
}

int main(int argc, char **argv)
{
    RUN(test_auto_hide_after_delay);
    RUN(test_hover_pauses_and_leaving_resets);
    RUN(test_autohide_off);
    RUN(test_cap_keeps_a_dismissible);
    RUN(test_dismiss_returns_pref);
    RUN(test_slow_cpu_refresh_once_per_document);
    RUN(test_io_warnings_status);
    return pc_t_finish();
}
