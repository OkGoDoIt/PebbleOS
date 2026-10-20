/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//! The watch's side of pairing the companion app with Muse: the "Pair with Muse?" prompt and the
//! status card while the window is open. Registers with the relay; a no-op without it.
void audio_companion_muse_ui_init(void);
