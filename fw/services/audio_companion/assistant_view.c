/* SPDX-License-Identifier: Apache-2.0 */

#include "assistant_view.h"

#include <string.h>

//! "…" in UTF-8, for an answer longer than the watch keeps.
static const char ELLIPSIS[] = "\xE2\x80\xA6";

//! The longest cut of text[0..length) at most `limit` bytes that ends on a character boundary:
//! steps back while the byte after the cut continues a character.
static size_t prv_utf8_cut(const char *text, size_t length, size_t limit) {
  if (limit >= length) {
    return length;
  }
  size_t cut = limit;
  while (cut > 0 && (((uint8_t)text[cut]) & 0xC0) == 0x80) {
    cut--;
  }
  return cut;
}

size_t assistant_view_copy_utf8(char *out, size_t out_size, const char *text, size_t length) {
  if (!out || out_size == 0) {
    return 0;
  }
  const size_t n = text ? prv_utf8_cut(text, length, out_size - 1) : 0;
  if (n) {
    memcpy(out, text, n);
  }
  out[n] = '\0';
  return n;
}

void assistant_view_begin(AudioCompanionAssistantView *view, uint8_t turn, uint16_t max_seconds) {
  const uint32_t generation = view->generation;
  memset(view, 0, sizeof(*view));
  view->generation = generation + 1;
  view->turn = turn;
  view->max_seconds = max_seconds;
  view->phase = AudioCompanionAssistantPhaseListening;
}

void assistant_view_clear(AudioCompanionAssistantView *view) {
  const uint32_t generation = view->generation;
  memset(view, 0, sizeof(*view));
  view->generation = generation + 1;
}

void assistant_view_set_phase(AudioCompanionAssistantView *view,
                              AudioCompanionAssistantPhase phase) {
  if (view->phase == phase) {
    return;
  }
  view->phase = phase;
  view->generation++;
}

void assistant_view_set_error(AudioCompanionAssistantView *view, uint8_t error) {
  view->error = error;
  view->phase = AudioCompanionAssistantPhaseError;
  view->generation++;
}

static void prv_append_answer(AudioCompanionAssistantView *view, const char *text, size_t length) {
  const size_t capacity = sizeof(view->answer) - 1;
  const size_t ellipsis = sizeof(ELLIPSIS) - 1;
  size_t used = strlen(view->answer);
  if (used + length <= capacity) {
    memcpy(view->answer + used, text, length);
    view->answer[used + length] = '\0';
    return;
  }
  // Too long: keep what fits with room for the ellipsis, on character boundaries.
  if (used + ellipsis > capacity) {
    used = prv_utf8_cut(view->answer, used, capacity - ellipsis);
  }
  const size_t keep = prv_utf8_cut(text, length, capacity - ellipsis - used);
  memcpy(view->answer + used, text, keep);
  memcpy(view->answer + used + keep, ELLIPSIS, ellipsis);
  view->answer[used + keep + ellipsis] = '\0';
  view->answer_truncated = true;
}

bool assistant_view_apply(AudioCompanionAssistantView *view, uint8_t turn, uint8_t kind,
                          uint8_t detail, const char *text, size_t length) {
  if (turn == 0 || turn != view->turn) {
    return false;
  }
  switch (kind) {
    case AudioCompanionAssistantUpdateHeard:
      assistant_view_copy_utf8(view->heard, sizeof(view->heard), text, length);
      if (view->phase == AudioCompanionAssistantPhaseSending ||
          view->phase == AudioCompanionAssistantPhaseListening) {
        view->phase = AudioCompanionAssistantPhaseWaiting;
      }
      break;
    case AudioCompanionAssistantUpdateStatus:
      assistant_view_copy_utf8(view->status, sizeof(view->status), text, length);
      if (view->phase == AudioCompanionAssistantPhaseSending) {
        view->phase = AudioCompanionAssistantPhaseWaiting;
      }
      break;
    case AudioCompanionAssistantUpdateAnswerRestart:
      if (view->phase == AudioCompanionAssistantPhaseError) {
        return false;
      }
      // The finished answer replaces what streamed; more may follow as Answer.
      view->answer[0] = '\0';
      view->answer_truncated = false;
      if (length) {
        prv_append_answer(view, text, length);
      }
      if (view->phase != AudioCompanionAssistantPhaseDone) {
        view->phase = AudioCompanionAssistantPhaseAnswering;
      }
      view->status[0] = '\0';
      break;
    case AudioCompanionAssistantUpdateAnswer:
      if (view->phase == AudioCompanionAssistantPhaseDone ||
          view->phase == AudioCompanionAssistantPhaseError || view->answer_truncated) {
        return false;
      }
      if (length) {
        prv_append_answer(view, text, length);
      }
      view->phase = AudioCompanionAssistantPhaseAnswering;
      view->status[0] = '\0';
      break;
    case AudioCompanionAssistantUpdateDone:
      if (view->phase == AudioCompanionAssistantPhaseError) {
        return false;
      }
      view->phase = AudioCompanionAssistantPhaseDone;
      view->status[0] = '\0';
      break;
    case AudioCompanionAssistantUpdateError:
      view->error = detail;
      view->phase = AudioCompanionAssistantPhaseError;
      assistant_view_copy_utf8(view->status, sizeof(view->status), text, length);
      break;
    default:
      return false;
  }
  view->generation++;
  return true;
}
