/* Budgets: preview/PNG ceilings and the incremental batch-reply budget (port of budgets.rs tests). */
#include "harness.h"

#include "automation/budgets.h"
#include "base/buf.h"

static pc_status cp(uint32_t w, uint32_t h, uint32_t s)
{
    return pc_auto_check_preview(w, h, s, NULL);
}

static void test_preview_checks(void)
{
    CHECK(cp(64, 32, 0) == PC_OK);
    CHECK(cp(PC_AUTO_MAX_PREVIEW_SIDE, PC_AUTO_MAX_PREVIEW_SIDE, 0) == PC_OK);
    CHECK(cp(PC_AUTO_MAX_PREVIEW_SIDE + 1, 1, 0) != PC_OK);
    CHECK(cp(64, 32, PC_AUTO_MAX_PREVIEW_SIDE + 1) != PC_OK);
    CHECK(cp(8192, 8192, 1024) == PC_OK);
    CHECK(cp(8193, 8192, 1024) != PC_OK);
    CHECK(cp(UINT32_MAX, UINT32_MAX, 1) != PC_OK);
    CHECK(cp(0, 32, 1) != PC_OK);
    CHECK(pc_auto_check_png(PC_AUTO_MAX_PNG_BYTES, NULL) == PC_OK);
    CHECK(pc_auto_check_png(PC_AUTO_MAX_PNG_BYTES + 1, NULL) != PC_OK);
}

static void test_batch_budget(void)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = yyjson_mut_strcpy(doc, "abc"); /* serialises as "abc" = 5 bytes */
    pc_auto_batch_budget b = {10, false};
    CHECK(pc_auto_batch_budget_charge(&b, v, NULL) == PC_OK);
    CHECK_EQ_UINT(b.remaining, 4u);
    CHECK(pc_auto_batch_budget_charge(&b, v, NULL) != PC_OK);
    /* Escaped charging counts the extra quote/backslash growth. */
    yyjson_mut_val *q = yyjson_mut_strcpy(doc, "a\"b"); /* "a\"b" -> plain 6, escaped 10 */
    pc_auto_batch_budget e = {11, true};
    CHECK(pc_auto_batch_budget_charge(&e, q, NULL) == PC_OK);
    CHECK_EQ_UINT(e.remaining, 0u);
    pc_auto_batch_budget e2 = {10, true};
    CHECK(pc_auto_batch_budget_charge(&e2, q, NULL) != PC_OK);
    yyjson_mut_doc_free(doc);
}

static void test_json_bytes(void)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = yyjson_mut_strcpy(doc, "hi");
    pc_buf out = PC_BUF_INIT;
    CHECK(pc_auto_json_bytes(v, &out, NULL) == PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "\"hi\"");
    pc_buf_fini(&out);
    yyjson_mut_doc_free(doc);
}

int main(int argc, char **argv)
{
    RUN(test_preview_checks);
    RUN(test_batch_budget);
    RUN(test_json_bytes);
    return pc_t_finish();
}
