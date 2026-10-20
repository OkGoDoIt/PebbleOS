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
