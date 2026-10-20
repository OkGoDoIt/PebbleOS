/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//! The watch's side of the assistant's watch actions: a timer ending, and a choice put to the
//! wearer. Registers with the audio companion service; a no-op without push-to-talk.
void audio_companion_actions_ui_init(void);
