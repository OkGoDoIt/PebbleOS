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

static void prv_clear_options(AudioCompanionAssistantView *view) {
  view->option_count = 0;
  memset(view->options, 0, sizeof(view->options));
}

bool assistant_view_apply(AudioCompanionAssistantView *view, uint8_t turn, uint8_t kind,
                          uint8_t detail, const char *text, size_t length) {
  if (turn == 0) {
    return false;
  }
  if (kind == AudioCompanionAssistantUpdateBegin) {
    // The receiver's own turn: a new exchange, its answer about to arrive.
    assistant_view_begin(view, turn, view->max_seconds);
    view->phase = AudioCompanionAssistantPhaseAnswering;
    view->from_receiver = true;
    if (detail == AudioCompanionAssistantBeginLateAnswer) {
      assistant_view_copy_utf8(view->heard, sizeof(view->heard), text, length);
    }
    return true;
  }
  if (turn != view->turn) {
    return false;
  }
  switch (kind) {
    case AudioCompanionAssistantUpdateHeard:
      if (view->phase == AudioCompanionAssistantPhaseDone) {
        // A follow-up on this screen (an option Muse offered, picked): it waits for its answer.
        view->answer[0] = '\0';
        view->answer_truncated = false;
        view->status[0] = '\0';
        prv_clear_options(view);
        view->phase = AudioCompanionAssistantPhaseWaiting;
      }
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
      // The finished answer replaces what streamed; more may follow as Answer. Its options come
      // after it.
      view->answer[0] = '\0';
      view->answer_truncated = false;
      prv_clear_options(view);
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
    case AudioCompanionAssistantUpdateOption:
      if (detail >= AUDIO_COMPANION_ASSISTANT_MAX_OPTIONS || length == 0 ||
          view->phase == AudioCompanionAssistantPhaseError) {
        return false;
      }
      assistant_view_copy_utf8(view->options[detail], sizeof(view->options[detail]), text,
                               length);
      if (detail >= view->option_count) {
        view->option_count = detail + 1;
      }
      break;
    default:
      return false;
  }
  view->generation++;
  return true;
}

void assistant_view_pick(AudioCompanionAssistantView *view, uint8_t index) {
  // What was picked is the wearer's reply: it waits for its answer, as a follow-up does.
  char picked[AUDIO_COMPANION_ASSISTANT_OPTION_BYTES];
  memcpy(picked, view->options[index], sizeof(picked));
  view->answer[0] = '\0';
  view->answer_truncated = false;
  view->status[0] = '\0';
  prv_clear_options(view);
  assistant_view_copy_utf8(view->heard, sizeof(view->heard), picked, strlen(picked));
  view->from_receiver = false;
  view->phase = AudioCompanionAssistantPhaseWaiting;
  view->generation++;
}

bool assistant_view_wants_notification(const AudioCompanionAssistantView *view) {
  switch (view->phase) {
    case AudioCompanionAssistantPhaseDone:
      return view->answer[0] != '\0';
    case AudioCompanionAssistantPhaseError:
      // "Still working" says the answer comes later; a lost phone is said by the watch already,
      // and its answer comes later too.
      return view->error != AudioCompanionAssistantErrorStillWorking &&
             view->error != AUDIO_COMPANION_ASSISTANT_ERROR_PHONE_UNAVAILABLE &&
             view->error != AUDIO_COMPANION_ASSISTANT_ERROR_MIC_UNAVAILABLE;
    default:
      return false;
  }
}

const char *assistant_view_error_sentence(uint8_t error) {
  switch (error) {
    case AudioCompanionAssistantErrorNotSetUp:
      return "Muse isn't set up. Set it up in Pebble Audio on your phone.";
    case AudioCompanionAssistantErrorUnreachable:
      return "Couldn't reach Muse. Check your phone's connection.";
    case AudioCompanionAssistantErrorDidNotCatch:
      return "Muse didn't catch that. Try asking again.";
    case AudioCompanionAssistantErrorBusy:
      return "Muse was busy. Try asking again.";
    case AudioCompanionAssistantErrorNoAnswer:
    default:
      return "Muse didn't answer. Try asking again.";
  }
}

//! Appends `text` to out[0..*used), cut on a character boundary; false when it didn't all fit.
static bool prv_append(char *out, size_t size, size_t *used, const char *text) {
  const size_t length = strlen(text);
  const size_t room = size - 1 - *used;
  const size_t n = prv_utf8_cut(text, length, room);
  memcpy(out + *used, text, n);
  *used += n;
  out[*used] = '\0';
  return n == length;
}

size_t assistant_view_notification_body(const AudioCompanionAssistantView *view, char *out,
                                        size_t size) {
  if (!out || size == 0) {
    return 0;
  }
  out[0] = '\0';
  size_t used = 0;
  if (view->phase == AudioCompanionAssistantPhaseError) {
    prv_append(out, size, &used, assistant_view_error_sentence(view->error));
    return used;
  }
  if (!prv_append(out, size, &used, view->answer)) {
    return used;
  }
  // The options read as a list after the answer; the notification's actions pick them.
  for (uint8_t i = 0; i < view->option_count; i++) {
    if (!view->options[i][0]) {
      continue;
    }
    if (!prv_append(out, size, &used, i == 0 ? "\n\n\xE2\x80\xA2 " : "\n\xE2\x80\xA2 ") ||
        !prv_append(out, size, &used, view->options[i])) {
      break;
    }
  }
  return used;
}
