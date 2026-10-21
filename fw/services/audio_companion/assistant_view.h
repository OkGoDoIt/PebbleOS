/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/services/audio_companion.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! What the watch shows for one question, built from ASSISTANT_UPDATE messages. Pure: the caller
//! holds whatever lock guards the view.

//! A new question: everything from the last one goes.
void assistant_view_begin(AudioCompanionAssistantView *view, uint8_t turn, uint16_t max_seconds);

//! Nothing to show, and no question: updates still arriving for the old one are ignored.
void assistant_view_clear(AudioCompanionAssistantView *view);

void assistant_view_set_phase(AudioCompanionAssistantView *view, AudioCompanionAssistantPhase phase);

void assistant_view_set_error(AudioCompanionAssistantView *view, uint8_t error);

//! Applies one update for `turn`. False when it changed nothing (another turn, or an answer
//! already cut short).
bool assistant_view_apply(AudioCompanionAssistantView *view, uint8_t turn, uint8_t kind,
                          uint8_t detail, const char *text, size_t length);

//! The wearer picked option `index` (< option_count): it becomes the question waiting for its
//! answer.
void assistant_view_pick(AudioCompanionAssistantView *view, uint8_t index);

//! Whether what the view shows, arriving while the screen is closed, deserves a notification: a
//! finished answer, or a question that failed for a reason the wearer can act on.
bool assistant_view_wants_notification(const AudioCompanionAssistantView *view);

//! One sentence for a failed question, for a notification.
const char *assistant_view_error_sentence(uint8_t error);

//! A notification's body for what the view shows: the answer and its options as a list, or the
//! failure. Cut on a character boundary to fit `size`. Returns the bytes written.
size_t assistant_view_notification_body(const AudioCompanionAssistantView *view, char *out,
                                        size_t size);

//! Copies `length` bytes of UTF-8 into `out` (size `out_size`, NUL-terminated), cut back to a
//! character boundary when it doesn't fit. Returns the bytes copied.
size_t assistant_view_copy_utf8(char *out, size_t out_size, const char *text, size_t length);
