/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//! The watch's side of the assistant's watch actions: a timer ending, and a choice put to the
//! wearer. Registers with the audio companion service; a no-op without push-to-talk.
void audio_companion_actions_ui_init(void);

#include "pbl/services/timeline/item.h"
#include "pbl/util/uuid.h"

#include <stdbool.h>

//! A notification of the assistant's (Muse's): opens the Muse app at its exchange and returns
//! true, so selecting it pulls the wearer back into the conversation rather than into the
//! notification's own view. False for any other notification.
bool audio_companion_assistant_open_from_notification(const TimelineItem *item);

//! The same, for a stored notification by id.
bool audio_companion_assistant_open_notification(const Uuid *id);
