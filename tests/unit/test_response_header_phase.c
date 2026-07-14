#include <stdbool.h>
#include <stdio.h>

#include "../../src/response_header_phase.h"

static int failures = 0;

#define ASSERT_INT(expected, actual) do { \
    int expected_value = (expected); \
    int actual_value = (actual); \
    if (expected_value != actual_value) { \
        fprintf(stderr, "%s:%d: expected %d, got %d\n", __FILE__, __LINE__, expected_value, actual_value); \
        failures++; \
    } \
} while (0)

#define ASSERT_BOOL(expected, actual) do { \
    bool expected_value = (expected); \
    bool actual_value = (actual); \
    if (expected_value != actual_value) { \
        fprintf(stderr, "%s:%d: expected %s, got %s\n", __FILE__, __LINE__, expected_value ? "true" : "false", actual_value ? "true" : "false"); \
        failures++; \
    } \
} while (0)

typedef enum {
    PHASE_OP_RESET,
    PHASE_OP_BEGIN,
    PHASE_OP_STATUS,
    PHASE_OP_END,
} phase_operation;

typedef struct {
    phase_operation operation;
    int http_status;
    grpc_response_header_block_phase expected_result;
    grpc_response_header_block_phase expected_current;
    bool expected_final_seen;
} phase_transition;

typedef struct {
    const char *name;
    const phase_transition *transitions;
    size_t transition_count;
} phase_transition_case;

#define TRANSITION_COUNT(transitions) (sizeof(transitions) / sizeof((transitions)[0]))

static const phase_transition direct_final_transitions[] = {
    {PHASE_OP_RESET, 0, GRPC_RESPONSE_HEADER_BLOCK_NONE, GRPC_RESPONSE_HEADER_BLOCK_NONE, false},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, false},
    {PHASE_OP_STATUS, 200, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, true},
    {PHASE_OP_END, 0, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, GRPC_RESPONSE_HEADER_BLOCK_NONE, true},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_TRAILING, GRPC_RESPONSE_HEADER_BLOCK_TRAILING, true},
    {PHASE_OP_END, 0, GRPC_RESPONSE_HEADER_BLOCK_TRAILING, GRPC_RESPONSE_HEADER_BLOCK_NONE, true},
};

static const phase_transition single_informational_transitions[] = {
    {PHASE_OP_RESET, 0, GRPC_RESPONSE_HEADER_BLOCK_NONE, GRPC_RESPONSE_HEADER_BLOCK_NONE, false},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, false},
    {PHASE_OP_STATUS, 103, GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, false},
    {PHASE_OP_END, 0, GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, GRPC_RESPONSE_HEADER_BLOCK_NONE, false},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, false},
    {PHASE_OP_STATUS, 200, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, true},
    {PHASE_OP_END, 0, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, GRPC_RESPONSE_HEADER_BLOCK_NONE, true},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_TRAILING, GRPC_RESPONSE_HEADER_BLOCK_TRAILING, true},
    {PHASE_OP_END, 0, GRPC_RESPONSE_HEADER_BLOCK_TRAILING, GRPC_RESPONSE_HEADER_BLOCK_NONE, true},
};

static const phase_transition repeated_informational_transitions[] = {
    {PHASE_OP_RESET, 0, GRPC_RESPONSE_HEADER_BLOCK_NONE, GRPC_RESPONSE_HEADER_BLOCK_NONE, false},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, false},
    {PHASE_OP_STATUS, 100, GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, false},
    {PHASE_OP_END, 0, GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, GRPC_RESPONSE_HEADER_BLOCK_NONE, false},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, false},
    {PHASE_OP_STATUS, 199, GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, false},
    {PHASE_OP_END, 0, GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, GRPC_RESPONSE_HEADER_BLOCK_NONE, false},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, false},
    {PHASE_OP_STATUS, 204, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, true},
    {PHASE_OP_END, 0, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, GRPC_RESPONSE_HEADER_BLOCK_NONE, true},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_TRAILING, GRPC_RESPONSE_HEADER_BLOCK_TRAILING, true},
};

static const phase_transition reset_for_reused_call_transitions[] = {
    {PHASE_OP_RESET, 0, GRPC_RESPONSE_HEADER_BLOCK_NONE, GRPC_RESPONSE_HEADER_BLOCK_NONE, false},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, false},
    {PHASE_OP_STATUS, 200, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, true},
    {PHASE_OP_END, 0, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, GRPC_RESPONSE_HEADER_BLOCK_NONE, true},
    {PHASE_OP_RESET, 0, GRPC_RESPONSE_HEADER_BLOCK_NONE, GRPC_RESPONSE_HEADER_BLOCK_NONE, false},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, false},
    {PHASE_OP_STATUS, 103, GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, false},
};

static const phase_transition ignored_status_transitions[] = {
    {PHASE_OP_RESET, 0, GRPC_RESPONSE_HEADER_BLOCK_NONE, GRPC_RESPONSE_HEADER_BLOCK_NONE, false},
    {PHASE_OP_STATUS, 103, GRPC_RESPONSE_HEADER_BLOCK_NONE, GRPC_RESPONSE_HEADER_BLOCK_NONE, false},
    {PHASE_OP_BEGIN, 0, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, false},
    {PHASE_OP_STATUS, 200, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, true},
    {PHASE_OP_STATUS, 103, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, true},
};

static void run_transition_case(const phase_transition_case *test_case)
{
    grpc_response_header_phase_state state = {
        GRPC_RESPONSE_HEADER_BLOCK_TRAILING,
        true,
    };

    for (size_t i = 0; i < test_case->transition_count; i++) {
        const phase_transition *transition = &test_case->transitions[i];
        grpc_response_header_block_phase result;

        switch (transition->operation) {
            case PHASE_OP_RESET:
                grpc_response_header_phase_reset(&state);
                result = state.block_phase;
                break;
            case PHASE_OP_BEGIN:
                result = grpc_response_header_phase_begin(&state);
                break;
            case PHASE_OP_STATUS:
                result = grpc_response_header_phase_on_status(&state, transition->http_status);
                break;
            case PHASE_OP_END:
                result = grpc_response_header_phase_end(&state);
                break;
            default:
                fprintf(stderr, "%s: unknown operation at transition %zu\n", test_case->name, i);
                failures++;
                continue;
        }

        if (result != transition->expected_result
            || state.block_phase != transition->expected_current
            || state.final_response_headers_seen != transition->expected_final_seen) {
            fprintf(stderr, "%s: transition %zu failed\n", test_case->name, i);
        }
        ASSERT_INT(transition->expected_result, result);
        ASSERT_INT(transition->expected_current, state.block_phase);
        ASSERT_BOOL(transition->expected_final_seen, state.final_response_headers_seen);
    }
}

static void test_block_role_predicates(void)
{
    static const struct {
        grpc_response_header_block_phase phase;
        bool allows_without_end_stream;
        bool allows_with_end_stream;
        bool trailing_without_status;
        bool trailing_after_status;
    } cases[] = {
        {GRPC_RESPONSE_HEADER_BLOCK_NONE, false, false, false, false},
        {GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, false, false, false, false},
        {GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, false, false, false, false},
        {GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, false, true, false, true},
        {GRPC_RESPONSE_HEADER_BLOCK_TRAILING, false, true, true, true},
    };
    grpc_response_header_phase_state state = {GRPC_RESPONSE_HEADER_BLOCK_NONE, false};

    for (size_t i = 0; i < TRANSITION_COUNT(cases); i++) {
        state.block_phase = cases[i].phase;
        ASSERT_BOOL(cases[i].allows_without_end_stream, grpc_response_header_phase_allows_status_fields(&state, false));
        ASSERT_BOOL(cases[i].allows_with_end_stream, grpc_response_header_phase_allows_status_fields(&state, true));
        ASSERT_BOOL(cases[i].trailing_without_status, grpc_response_header_phase_metadata_is_trailing(&state, false));
        ASSERT_BOOL(cases[i].trailing_after_status, grpc_response_header_phase_metadata_is_trailing(&state, true));
    }
    ASSERT_BOOL(false, grpc_response_header_phase_allows_status_fields(NULL, true));
    ASSERT_BOOL(false, grpc_response_header_phase_metadata_is_trailing(NULL, true));
}

int main(void)
{
    static const phase_transition_case cases[] = {
        {"direct final then trailing", direct_final_transitions, TRANSITION_COUNT(direct_final_transitions)},
        {"single informational then final", single_informational_transitions, TRANSITION_COUNT(single_informational_transitions)},
        {"repeated informational then final", repeated_informational_transitions, TRANSITION_COUNT(repeated_informational_transitions)},
        {"reset for reused call", reset_for_reused_call_transitions, TRANSITION_COUNT(reset_for_reused_call_transitions)},
        {"status only transitions awaiting block", ignored_status_transitions, TRANSITION_COUNT(ignored_status_transitions)},
    };

    for (size_t i = 0; i < TRANSITION_COUNT(cases); i++) {
        run_transition_case(&cases[i]);
    }
    test_block_role_predicates();

    if (failures != 0) {
        fprintf(stderr, "%d response header phase unit assertions failed\n", failures);
        return 1;
    }
    puts("response header phase unit tests passed");
    return 0;
}
