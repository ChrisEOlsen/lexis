/* Tests for the streaming think gate: streamed bytes must equal strip_leading_think_block().
 * Drives think_gate_feed() with no model via local_llm_client_test.h hooks. */

#include "test_utils.h"

#include "local_llm_client_test.h"

#include <stdio.h>
#include <string.h>

#define MAX_REPLY 4096

typedef struct {
    char data[MAX_REPLY];
    size_t len;
} Sink;

static void sink_write(const char *piece, size_t piece_len, void *user_data) {
    Sink *sink = user_data;
    if (sink->len + piece_len >= MAX_REPLY) {
        return; /* test inputs are small; a overflow here is a broken test */
    }
    memcpy(sink->data + sink->len, piece, piece_len);
    sink->len += piece_len;
    sink->data[sink->len] = '\0';
}

static void gate_init(ThinkGate *gate) {
    gate->phase_deciding = 1;
    gate->phase_inside = 0;
    gate->format = 0;
    for (size_t f = 0; f < THINK_GATE_FORMAT_COUNT; f++) {
        gate->open_match[f] = 0;
    }
    gate->close_scanned = 0;
    gate->skip_leading_ws = 0;
    gate->held.data = NULL;
    gate->held.length = 0;
    gate->held.capacity = 0;
}

/* Feed pieces through the gate; streamed bytes must equal the stripped reply. */
static void check_matches_strip(const char *name, const char *const *pieces, size_t count) {
    ThinkGate gate;
    gate_init(&gate);

    Sink sink;
    sink.data[0] = '\0';
    sink.len = 0;

    char whole[MAX_REPLY];
    whole[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        strncat(whole, pieces[i], MAX_REPLY - strlen(whole) - 1);
        think_gate_feed(&gate, pieces[i], sink_write, &sink);
    }
    think_gate_flush(&gate, sink_write, &sink);

    strip_leading_think_block(whole);
    TEST_ASSERT(strcmp(sink.data, whole) == 0, "%s: streamed \"%s\", returned \"%s\"", name, sink.data, whole);
}

static void test_close_marker_ends_a_piece(void) {
    /* Trailing whitespace in the next piece must be eaten too. */
    static const char *const pieces[] = {"<think>", "reasoning here", "</think>", "\n\n", "The answer."};
    check_matches_strip("close marker ends a piece", pieces, 5);
}

static void test_whitespace_run_split_across_pieces(void) {
    static const char *const pieces[] = {"<think>a</think>", "\n", "\n", "  ", "The answer."};
    check_matches_strip("whitespace run split across pieces", pieces, 5);
}

static void test_close_and_whitespace_in_one_piece(void) {
    static const char *const pieces[] = {"<think>reasoning</think>\n\n", "The answer."};
    check_matches_strip("close and whitespace in one piece", pieces, 2);
}

static void test_whole_reply_in_one_piece(void) {
    /* Open and close in one piece must still scan for the close marker. */
    static const char *const pieces[] = {"<think>reasoning</think>\n\nThe answer."};
    check_matches_strip("whole reply in one piece", pieces, 1);
}

static void test_nothing_but_whitespace_after_close(void) {
    static const char *const pieces[] = {"<think>a</think>", "\n\n"};
    check_matches_strip("nothing but whitespace after close", pieces, 2);
}

static void test_ordinary_answer_streams_unchanged(void) {
    static const char *const pieces[] = {"An ordinary ", "answer with no think block."};
    check_matches_strip("ordinary answer", pieces, 2);
}

static void test_answer_opening_with_a_lookalike(void) {
    /* Lookalike prefix ruled out: held bytes must be released. */
    static const char *const pieces[] = {"<thi", "ngs like this>", " are not think blocks."};
    check_matches_strip("answer that looks like a marker at first", pieces, 3);
}

static void test_opened_but_never_closed(void) {
    /* Truncated mid-thought stays visible, not silently emptied. */
    static const char *const pieces[] = {"<think>", "truncated mid-thought"};
    check_matches_strip("opened but never closed", pieces, 2);
}

static void test_gemma_markers(void) {
    static const char *const pieces[] = {"<|channel>thought", " gemma reasoning ", "<channel|>", "\n", "Answer."};
    check_matches_strip("gemma markers split across pieces", pieces, 5);
}

static void test_leading_whitespace_in_an_ordinary_answer_is_kept(void) {
    /* No think block means leading whitespace is kept. */
    static const char *const pieces[] = {"\n", "  Answer with leading whitespace."};
    check_matches_strip("ordinary answer keeps its leading whitespace", pieces, 2);
}

int main(void) {
    test_close_marker_ends_a_piece();
    test_whitespace_run_split_across_pieces();
    test_close_and_whitespace_in_one_piece();
    test_whole_reply_in_one_piece();
    test_nothing_but_whitespace_after_close();
    test_ordinary_answer_streams_unchanged();
    test_answer_opening_with_a_lookalike();
    test_opened_but_never_closed();
    test_gemma_markers();
    test_leading_whitespace_in_an_ordinary_answer_is_kept();
    return test_summary();
}
