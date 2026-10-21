/* SPDX-License-Identifier: Apache-2.0 */

#include "services/audio_companion/assistant_view.h"

#include "clar.h"

#include <string.h>

static AudioCompanionAssistantView s_view;

static bool prv_apply(uint8_t turn, uint8_t kind, uint8_t detail, const char *text) {
  return assistant_view_apply(&s_view, turn, kind, detail, text, text ? strlen(text) : 0);
}

void test_audio_companion_assistant_view__initialize(void) {
  memset(&s_view, 0, sizeof(s_view));
}

void test_audio_companion_assistant_view__begin_starts_listening_with_nothing_left_over(void) {
  assistant_view_begin(&s_view, 3, 15);
  prv_apply(3, AudioCompanionAssistantUpdateAnswer, 0, "old answer");
  const uint32_t generation = s_view.generation;

  assistant_view_begin(&s_view, 4, 15);
  cl_assert_equal_i(s_view.turn, 4);
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseListening);
  cl_assert_equal_i(s_view.max_seconds, 15);
  cl_assert_equal_s(s_view.answer, "");
  cl_assert(s_view.generation > generation);
}

void test_audio_companion_assistant_view__updates_for_another_question_are_ignored(void) {
  assistant_view_begin(&s_view, 5, 15);
  const uint32_t generation = s_view.generation;
  cl_assert(!prv_apply(4, AudioCompanionAssistantUpdateAnswer, 0, "late"));
  cl_assert(!prv_apply(0, AudioCompanionAssistantUpdateAnswer, 0, "nobody's"));
  cl_assert_equal_s(s_view.answer, "");
  cl_assert_equal_i(s_view.generation, generation);
}

void test_audio_companion_assistant_view__a_question_runs_heard_status_answer_done(void) {
  assistant_view_begin(&s_view, 1, 15);
  assistant_view_set_phase(&s_view, AudioCompanionAssistantPhaseSending);

  cl_assert(prv_apply(1, AudioCompanionAssistantUpdateHeard, 0, "what time is it in Tokyo"));
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseWaiting);
  cl_assert_equal_s(s_view.heard, "what time is it in Tokyo");

  cl_assert(prv_apply(1, AudioCompanionAssistantUpdateStatus, 0, "Checking"));
  cl_assert_equal_s(s_view.status, "Checking");
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseWaiting);

  cl_assert(prv_apply(1, AudioCompanionAssistantUpdateAnswer, 0, "It's 9:40 "));
  cl_assert(prv_apply(1, AudioCompanionAssistantUpdateAnswer, 0, "in the morning."));
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseAnswering);
  cl_assert_equal_s(s_view.answer, "It's 9:40 in the morning.");
  cl_assert_equal_s(s_view.status, "");  // the answer replaces "what Muse is doing"

  cl_assert(prv_apply(1, AudioCompanionAssistantUpdateDone, 0, NULL));
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseDone);
  // Nothing more is added to a finished answer.
  cl_assert(!prv_apply(1, AudioCompanionAssistantUpdateAnswer, 0, " Also..."));
  cl_assert_equal_s(s_view.answer, "It's 9:40 in the morning.");
}

void test_audio_companion_assistant_view__an_error_sticks(void) {
  assistant_view_begin(&s_view, 2, 15);
  cl_assert(prv_apply(2, AudioCompanionAssistantUpdateError, AudioCompanionAssistantErrorNotSetUp,
                      ""));
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseError);
  cl_assert_equal_i(s_view.error, AudioCompanionAssistantErrorNotSetUp);
  cl_assert(!prv_apply(2, AudioCompanionAssistantUpdateDone, 0, NULL));
  cl_assert(!prv_apply(2, AudioCompanionAssistantUpdateAnswer, 0, "too late"));
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseError);
}

void test_audio_companion_assistant_view__a_long_answer_ends_in_an_ellipsis_on_a_boundary(void) {
  assistant_view_begin(&s_view, 1, 15);
  // "é" is two bytes: 180-byte chunks of it can only be cut between characters.
  char chunk[AUDIO_COMPANION_ASSISTANT_TEXT_MAX_BYTES + 1];
  for (size_t i = 0; i + 1 < sizeof(chunk); i += 2) {
    chunk[i] = (char)0xC3;
    chunk[i + 1] = (char)0xA9;
  }
  chunk[sizeof(chunk) - 1] = '\0';
  int applied = 0;
  for (int i = 0; i < 20; i++) {
    applied += prv_apply(1, AudioCompanionAssistantUpdateAnswer, 0, chunk) ? 1 : 0;
  }
  cl_assert(s_view.answer_truncated);
  cl_assert(applied < 20);  // once cut short, later pieces change nothing
  const size_t length = strlen(s_view.answer);
  cl_assert(length < sizeof(s_view.answer));
  cl_assert_equal_s(s_view.answer + length - 3, "\xE2\x80\xA6");
  // Every byte before the ellipsis is part of a whole "é".
  for (size_t i = 0; i < length - 3; i += 2) {
    cl_assert_equal_i((uint8_t)s_view.answer[i], 0xC3);
    cl_assert_equal_i((uint8_t)s_view.answer[i + 1], 0xA9);
  }
}

void test_audio_companion_assistant_view__clear_drops_late_words_about_a_closed_question(void) {
  assistant_view_begin(&s_view, 7, 15);
  prv_apply(7, AudioCompanionAssistantUpdateHeard, 0, "set a timer");
  assistant_view_clear(&s_view);
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseIdle);
  cl_assert_equal_i(s_view.turn, 0);
  cl_assert_equal_s(s_view.heard, "");
  cl_assert(!prv_apply(7, AudioCompanionAssistantUpdateAnswer, 0, "Timer set."));
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseIdle);
}

void test_audio_companion_assistant_view__copy_cuts_between_characters(void) {
  char out[4];
  // "aé€" = 1 + 2 + 3 bytes; four bytes of room fit "aé" and a terminator.
  const char *text = "a\xC3\xA9\xE2\x82\xAC";
  cl_assert_equal_i(assistant_view_copy_utf8(out, sizeof(out), text, strlen(text)), 3);
  cl_assert_equal_s(out, "a\xC3\xA9");
  cl_assert_equal_i(assistant_view_copy_utf8(out, sizeof(out), NULL, 5), 0);
  cl_assert_equal_s(out, "");
}

void test_audio_companion_assistant_view__a_restart_replaces_what_streamed(void) {
  assistant_view_begin(&s_view, 1, 15);
  prv_apply(1, AudioCompanionAssistantUpdateAnswer, 0, "1. 09:53 your first note");
  cl_assert(prv_apply(1, AudioCompanionAssistantUpdateAnswerRestart, 0, "1. 09:53 - your 1st"));
  prv_apply(1, AudioCompanionAssistantUpdateAnswer, 0, " voice note");
  cl_assert_equal_s(s_view.answer, "1. 09:53 - your 1st voice note");
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseAnswering);
  prv_apply(1, AudioCompanionAssistantUpdateDone, 0, NULL);
  // A late restart still corrects a finished answer, and leaves it finished.
  cl_assert(prv_apply(1, AudioCompanionAssistantUpdateAnswerRestart, 0, "Final."));
  cl_assert_equal_s(s_view.answer, "Final.");
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseDone);
}


void test_audio_companion_assistant_view__heard_after_done_is_a_follow_up(void) {
  assistant_view_begin(&s_view, 2, 15);
  prv_apply(2, AudioCompanionAssistantUpdateHeard, 0, "Ask me a question with A, B, C");
  prv_apply(2, AudioCompanionAssistantUpdateAnswer, 0, "What is 2 times 7?\nA: 12\nB: 14");
  prv_apply(2, AudioCompanionAssistantUpdateDone, 0, NULL);
  // The option picked on the watch goes to Muse: it shows as asked, and its answer is awaited.
  cl_assert(prv_apply(2, AudioCompanionAssistantUpdateHeard, 0, "B: 14"));
  cl_assert_equal_s(s_view.heard, "B: 14");
  cl_assert_equal_s(s_view.answer, "");
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseWaiting);
  prv_apply(2, AudioCompanionAssistantUpdateAnswer, 0, "Right, 14.");
  prv_apply(2, AudioCompanionAssistantUpdateDone, 0, NULL);
  cl_assert_equal_s(s_view.answer, "Right, 14.");
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseDone);
}

void test_audio_companion_assistant_view__options_come_with_the_answer_and_go_with_it(void) {
  assistant_view_begin(&s_view, 4, 15);
  cl_assert(prv_apply(4, AudioCompanionAssistantUpdateAnswer, 0, "Which one?"));
  cl_assert(prv_apply(4, AudioCompanionAssistantUpdateOption, 0, "Seven"));
  cl_assert(prv_apply(4, AudioCompanionAssistantUpdateOption, 1, "Twelve"));
  // Past the last slot, or empty: not an option.
  cl_assert(!prv_apply(4, AudioCompanionAssistantUpdateOption, AUDIO_COMPANION_ASSISTANT_MAX_OPTIONS,
                       "Nine"));
  cl_assert(!prv_apply(4, AudioCompanionAssistantUpdateOption, 2, ""));
  cl_assert(prv_apply(4, AudioCompanionAssistantUpdateDone, 0, ""));
  cl_assert_equal_i(s_view.option_count, 2);
  cl_assert_equal_s(s_view.options[1], "Twelve");

  // The body lists them after the answer.
  char body[256];
  assistant_view_notification_body(&s_view, body, sizeof(body));
  cl_assert_equal_s(body, "Which one?\n\n\xE2\x80\xA2 Seven\n\xE2\x80\xA2 Twelve");
  cl_assert(assistant_view_wants_notification(&s_view));

  // Picking one makes it the question waiting for its answer.
  assistant_view_pick(&s_view, 1);
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseWaiting);
  cl_assert_equal_s(s_view.heard, "Twelve");
  cl_assert_equal_s(s_view.answer, "");
  cl_assert_equal_i(s_view.option_count, 0);
  cl_assert(!assistant_view_wants_notification(&s_view));

  // A finished answer that replaces what streamed brings its own options, if any.
  cl_assert(prv_apply(4, AudioCompanionAssistantUpdateAnswer, 0, "Right"));
  cl_assert(prv_apply(4, AudioCompanionAssistantUpdateOption, 0, "Again"));
  cl_assert(prv_apply(4, AudioCompanionAssistantUpdateAnswerRestart, 0, "Right!"));
  cl_assert_equal_i(s_view.option_count, 0);
}

void test_audio_companion_assistant_view__the_receiver_begins_its_own_exchange(void) {
  assistant_view_begin(&s_view, 4, 15);
  cl_assert(prv_apply(4, AudioCompanionAssistantUpdateAnswer, 0, "Old."));
  // A message Muse sent by itself: no question.
  cl_assert(prv_apply(9, AudioCompanionAssistantUpdateBegin, AudioCompanionAssistantBeginMessage,
                      "ignored"));
  cl_assert_equal_i(s_view.turn, 9);
  cl_assert_equal_i(s_view.phase, AudioCompanionAssistantPhaseAnswering);
  cl_assert(s_view.from_receiver);
  cl_assert_equal_s(s_view.heard, "");
  cl_assert_equal_s(s_view.answer, "");
  cl_assert_equal_i(s_view.max_seconds, 15);
  // A late answer: its question is shown above it.
  cl_assert(prv_apply(10, AudioCompanionAssistantUpdateBegin,
                      AudioCompanionAssistantBeginLateAnswer, "what's the weather"));
  cl_assert_equal_s(s_view.heard, "what's the weather");
  cl_assert(prv_apply(10, AudioCompanionAssistantUpdateAnswer, 0, "Sunny."));
  cl_assert_equal_s(s_view.answer, "Sunny.");
}

void test_audio_companion_assistant_view__which_endings_are_worth_a_notification(void) {
  assistant_view_begin(&s_view, 4, 15);
  cl_assert(prv_apply(4, AudioCompanionAssistantUpdateDone, 0, ""));
  // Done with no words: nothing to say.
  cl_assert(!assistant_view_wants_notification(&s_view));

  char body[128];
  assistant_view_begin(&s_view, 5, 15);
  cl_assert(prv_apply(5, AudioCompanionAssistantUpdateError, AudioCompanionAssistantErrorNoAnswer,
                      ""));
  cl_assert(assistant_view_wants_notification(&s_view));
  assistant_view_notification_body(&s_view, body, sizeof(body));
  cl_assert_equal_s(body, "Muse didn't answer. Try asking again.");

  assistant_view_begin(&s_view, 6, 15);
  cl_assert(prv_apply(6, AudioCompanionAssistantUpdateError,
                      AudioCompanionAssistantErrorStillWorking, ""));
  cl_assert(!assistant_view_wants_notification(&s_view));
  assistant_view_set_error(&s_view, AUDIO_COMPANION_ASSISTANT_ERROR_PHONE_UNAVAILABLE);
  cl_assert(!assistant_view_wants_notification(&s_view));
}

void test_audio_companion_assistant_view__a_long_body_is_cut_between_characters(void) {
  assistant_view_begin(&s_view, 4, 15);
  // "é" is two bytes: a 6-byte buffer holds "aé" and the NUL, never half of the second "é".
  cl_assert(prv_apply(4, AudioCompanionAssistantUpdateAnswer, 0, "a\xC3\xA9\xC3\xA9"));
  cl_assert(prv_apply(4, AudioCompanionAssistantUpdateDone, 0, ""));
  char body[5];
  cl_assert_equal_i(assistant_view_notification_body(&s_view, body, sizeof(body)), 3);
  cl_assert_equal_s(body, "a\xC3\xA9");
}
