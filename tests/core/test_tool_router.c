/* Tests for tool_router.c. No model needed: the failure path is exercised uninitialized. */

#include "tool_router.h"
#include "test_utils.h"

static void test_uninitialized_model_reports_failure(void) {
    int model_failed = 0;
    ToolChoice choice = tool_router_choose_tool("hello", NULL, 0, 0, &model_failed);
    TEST_ASSERT(choice == TOOL_SEARCH_PASSAGES, "expected SEARCH fallback, got %d", choice);
    TEST_ASSERT(model_failed == 1, "expected model_failed=1 without an initialized model");
}

static void test_null_out_param_is_safe(void) {
    ToolChoice choice = tool_router_choose_tool("hello", NULL, 0, 0, NULL);
    TEST_ASSERT(choice == TOOL_SEARCH_PASSAGES, "expected SEARCH fallback, got %d", choice);
}

int main(void) {
    test_uninitialized_model_reports_failure();
    test_null_out_param_is_safe();
    return test_summary();
}
