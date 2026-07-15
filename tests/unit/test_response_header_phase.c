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
        .block_phase = GRPC_RESPONSE_HEADER_BLOCK_TRAILING,
        .final_response_headers_seen = true,
        .trailers_only_candidate = false,
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
        bool trailing_without_terminal_status;
        bool trailing_after_terminal_status;
    } cases[] = {
        {GRPC_RESPONSE_HEADER_BLOCK_NONE, false, false, false, false},
        {GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, false, false, false, false},
        {GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, false, false, false, false},
        {GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL, false, true, false, true},
        {GRPC_RESPONSE_HEADER_BLOCK_TRAILING, false, true, true, true},
    };
    grpc_response_header_phase_state state = {GRPC_RESPONSE_HEADER_BLOCK_NONE, false, false};

    for (size_t i = 0; i < TRANSITION_COUNT(cases); i++) {
        state.block_phase = cases[i].phase;
        state.trailers_only_candidate = false;
        ASSERT_BOOL(cases[i].allows_without_end_stream, grpc_response_header_phase_allows_status_fields(&state, false));
        ASSERT_BOOL(cases[i].allows_with_end_stream, grpc_response_header_phase_allows_status_fields(&state, true));
        ASSERT_BOOL(cases[i].trailing_without_terminal_status, grpc_response_header_phase_metadata_is_trailing(&state));
        state.trailers_only_candidate = true;
        ASSERT_BOOL(cases[i].trailing_after_terminal_status, grpc_response_header_phase_metadata_is_trailing(&state));
    }
    ASSERT_BOOL(false, grpc_response_header_phase_allows_status_fields(NULL, true));
    ASSERT_BOOL(false, grpc_response_header_phase_on_trailers_only_status_field(NULL, true));
    ASSERT_BOOL(false, grpc_response_header_phase_metadata_is_trailing(NULL));
}

static void test_terminal_status_field_role_transition(void)
{
    grpc_response_header_phase_state state;

    grpc_response_header_phase_reset(&state);
    grpc_response_header_phase_begin(&state);
    grpc_response_header_phase_on_status(&state, 103);
    ASSERT_BOOL(false, grpc_response_header_phase_on_trailers_only_status_field(&state, true));
    ASSERT_BOOL(false, grpc_response_header_phase_metadata_is_trailing(&state));
    grpc_response_header_phase_end(&state);

    grpc_response_header_phase_begin(&state);
    grpc_response_header_phase_on_status(&state, 200);
    ASSERT_BOOL(false, grpc_response_header_phase_on_trailers_only_status_field(&state, false));
    ASSERT_BOOL(false, grpc_response_header_phase_metadata_is_trailing(&state));
    ASSERT_BOOL(true, grpc_response_header_phase_on_trailers_only_status_field(&state, true));
    ASSERT_BOOL(true, grpc_response_header_phase_metadata_is_trailing(&state));
    ASSERT_BOOL(false, grpc_response_header_phase_on_trailers_only_status_field(&state, true));
    ASSERT_BOOL(true, grpc_response_header_phase_metadata_is_trailing(&state));
    grpc_response_header_phase_end(&state);
    ASSERT_BOOL(false, state.trailers_only_candidate);

    grpc_response_header_phase_begin(&state);
    ASSERT_INT(GRPC_RESPONSE_HEADER_BLOCK_TRAILING, state.block_phase);
    ASSERT_BOOL(false, grpc_response_header_phase_on_trailers_only_status_field(&state, true));
    ASSERT_BOOL(true, grpc_response_header_phase_metadata_is_trailing(&state));
    grpc_response_header_phase_reset(&state);
    ASSERT_BOOL(false, state.trailers_only_candidate);
}

static void test_abandoned_open_block_requires_connection_terminal(void)
{
    grpc_response_header_phase_state state;

    ASSERT_BOOL(false, grpc_response_header_phase_requires_connection_terminal_on_abandonment(NULL));
    for (int phase = GRPC_RESPONSE_HEADER_BLOCK_NONE; phase <= GRPC_RESPONSE_HEADER_BLOCK_TRAILING; phase++) {
        state.block_phase = (grpc_response_header_block_phase) phase;
        ASSERT_BOOL(
            phase != GRPC_RESPONSE_HEADER_BLOCK_NONE,
            grpc_response_header_phase_requires_connection_terminal_on_abandonment(&state)
        );
    }

    grpc_response_header_phase_reset(&state);
    ASSERT_BOOL(false, grpc_response_header_phase_requires_connection_terminal_on_abandonment(&state));
    ASSERT_INT(GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS, grpc_response_header_phase_begin(&state));
    ASSERT_BOOL(true, grpc_response_header_phase_requires_connection_terminal_on_abandonment(&state));
    ASSERT_INT(GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, grpc_response_header_phase_on_status(&state, 103));
    ASSERT_BOOL(true, grpc_response_header_phase_requires_connection_terminal_on_abandonment(&state));
    ASSERT_INT(GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL, grpc_response_header_phase_end(&state));
    ASSERT_BOOL(false, grpc_response_header_phase_requires_connection_terminal_on_abandonment(&state));
}

static void test_field_classification(void)
{
    ASSERT_INT(GRPC_RESPONSE_HEADER_FIELD_REJECTED, grpc_response_header_classify_reported_field(NULL, 0, false));
    ASSERT_INT(GRPC_RESPONSE_HEADER_FIELD_REJECTED, grpc_response_header_classify_reported_field((const uint8_t *) "", 0, false));
    ASSERT_INT(GRPC_RESPONSE_HEADER_FIELD_INVALID_REGULAR, grpc_response_header_classify_reported_field((const uint8_t *) "", 0, true));
    ASSERT_INT(GRPC_RESPONSE_HEADER_FIELD_STATUS, grpc_response_header_classify_reported_field((const uint8_t *) ":status", sizeof(":status") - 1, false));
    ASSERT_INT(GRPC_RESPONSE_HEADER_FIELD_REJECTED, grpc_response_header_classify_reported_field((const uint8_t *) ":status", sizeof(":status") - 1, true));
    ASSERT_INT(GRPC_RESPONSE_HEADER_FIELD_REJECTED, grpc_response_header_classify_reported_field((const uint8_t *) ":foo", sizeof(":foo") - 1, false));
    ASSERT_INT(GRPC_RESPONSE_HEADER_FIELD_REGULAR, grpc_response_header_classify_reported_field((const uint8_t *) "x-field", sizeof("x-field") - 1, false));
    ASSERT_INT(GRPC_RESPONSE_HEADER_FIELD_INVALID_REGULAR, grpc_response_header_classify_reported_field((const uint8_t *) "x-field", sizeof("x-field") - 1, true));
}

static void test_field_class_phase_routes(void)
{
    static const struct {
        grpc_response_header_field_class field_class;
        grpc_response_header_field_route expected[5];
    } cases[] = {
        {
            GRPC_RESPONSE_HEADER_FIELD_STATUS,
            {
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_STATUS,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
            },
        },
        {
            GRPC_RESPONSE_HEADER_FIELD_REGULAR,
            {
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_IGNORE,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_PROCESS,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_PROCESS,
            },
        },
        {
            GRPC_RESPONSE_HEADER_FIELD_INVALID_REGULAR,
            {
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_IGNORE,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_IGNORE,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_IGNORE,
            },
        },
        {
            GRPC_RESPONSE_HEADER_FIELD_REJECTED,
            {
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
            },
        },
        {
            (grpc_response_header_field_class) 99,
            {
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
                GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR,
            },
        },
    };

    for (size_t field_index = 0; field_index < TRANSITION_COUNT(cases); field_index++) {
        for (int phase = GRPC_RESPONSE_HEADER_BLOCK_NONE; phase <= GRPC_RESPONSE_HEADER_BLOCK_TRAILING; phase++) {
            ASSERT_INT(
                cases[field_index].expected[phase],
                grpc_response_header_route_field(
                    (grpc_response_header_block_phase) phase,
                    cases[field_index].field_class
                )
            );
        }
    }
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
    test_terminal_status_field_role_transition();
    test_abandoned_open_block_requires_connection_terminal();
    test_field_classification();
    test_field_class_phase_routes();

    if (failures != 0) {
        fprintf(stderr, "%d response header phase unit assertions failed\n", failures);
        return 1;
    }
    puts("response header phase unit tests passed");
    return 0;
}
