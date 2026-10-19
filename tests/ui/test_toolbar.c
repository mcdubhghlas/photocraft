#include "harness.h"
#include "ui/toolbar.h"
#include <string.h>
static void test_slots_partition_tools_and_remember_selection(void)
{
    bool seen[PC_TOOL_N] = {0};
    size_t count = 0;
    pc_ui_state state;
    pc_ui_state_init(&state);
    CHECK(pc_ui_toolbar_slots() == 21);
    unsigned previous = 0;
    for (size_t s = 0; s < pc_ui_toolbar_slots(); s++) {
        size_t n = 0;
        unsigned section = 0;
        const pc_ui_tool *tools = pc_ui_toolbar_slot(s, &n, &section);
        CHECK(tools && n > 0 && n <= 6 && section >= previous && section <= 4);
        previous = section;
        for (size_t i = 0; i < n; i++) {
            CHECK(tools[i] < PC_TOOL_N && !seen[tools[i]]);
            seen[tools[i]] = true;
            count++;
            state.tool = tools[i];
            CHECK(pc_ui_toolbar_current(&state, s) == tools[i]);
        }
        state.tool = tools[n - 1];
        pc_ui_state_remember_group_tool(&state);
        state.tool = PC_TOOL_ZOOM;
        if (s != 20) {
            CHECK(pc_ui_toolbar_current(&state, s) == tools[n - 1]);
        }
    }
    CHECK(count == PC_TOOL_N);
    size_t n = 99;
    CHECK(pc_ui_toolbar_slot(100, &n, NULL) == NULL && n == 0);
    pc_ui_state_fini(&state);
}
int main(int argc, char **argv)
{
    RUN(test_slots_partition_tools_and_remember_selection);
    return pc_t_finish();
}
