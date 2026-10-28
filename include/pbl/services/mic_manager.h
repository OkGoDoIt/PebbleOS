/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/drivers/mic.h>

#include <kernel/pebble_tasks.h>

//! Arbitrates the single microphone between kernel clients.
//!
//! Dictation (the system voice UI) has priority over app capture: acquiring for dictation while an
//! app owns the mic preempts the app, which is told through its preempted callback before the mic
//! is restarted. An app cannot acquire the mic while dictation owns it. The OS mic banner is shown
//! while an app client owns the running mic.
//!
//! A background client records only while nobody else wants the mic: dictation and apps take it
//! away like dictation takes it from an app, a dictation session stops it as soon as it begins,
//! and it is told once the mic is free again. While it holds the mic for a user action, apps are
//! refused as they are by any other owner. It never shows the banner.

typedef enum MicClient {
  MicClientNone = 0,
  MicClientVoiceDictation,
  MicClientAppStream,
  MicClientAppCapture,
  MicClientBackground,
} MicClient;

typedef enum MicManagerAppCheck {
  MicManagerAppOk = 0,
  MicManagerAppNotForeground,
  MicManagerAppWatchface,
} MicManagerAppCheck;

//! Whether `task` is a foreground app in focus, the only kind that may use the mic.
MicManagerAppCheck mic_manager_check_app(PebbleTask task);

//! Called (without the manager lock held) when the client's mic is taken away. The client must
//! not call mic_manager_release() for the lost session; it is already released.
typedef void (*MicManagerPreemptedCb)(void *context);

//! Called (without the manager lock held, on the task that freed the mic) once the mic is free
//! after the background client was refused or preempted. It must not block or call the manager.
typedef void (*MicManagerFreeCb)(void *context);

void mic_manager_init(void);

//! Starts the microphone for a client. See \ref mic_start for the buffer semantics.
//! @return true if the client now owns the running microphone
bool mic_manager_acquire(MicClient client, MicDataHandlerCB handler, void *context, int16_t *buffer,
                         size_t buffer_len, MicManagerPreemptedCb on_preempted,
                         void *preempt_context);

//! Holds the idle mic for an app client whose start is pending, so other apps see it as busy.
//! Only that client or dictation may then acquire it. Cleared by acquire or release.
//! @return true if the mic was idle and unreserved
bool mic_manager_reserve(MicClient client);

//! Stops the microphone if the client owns it and drops its reservation. No-op otherwise.
void mic_manager_release(MicClient client);

//! Set for a whole dictation session, from before it takes the audio endpoint until it ends. Apps
//! can neither reserve nor acquire the mic meanwhile.
void mic_manager_set_dictation_active(bool active);

MicClient mic_manager_get_owner(void);

//! @return true while the mic is owned or reserved, or a dictation session runs. A background
//! owner counts only while it holds the mic.
bool mic_manager_is_busy(void);

//! Sets who to tell when the mic is free again for the background client.
void mic_manager_set_background_free_cb(MicManagerFreeCb cb, void *context);

//! While set, apps can neither take the mic from the background client nor reserve it, and it
//! counts as busy; dictation still takes it. Has effect only while the background client owns
//! the mic; the client clears it when the user action ends.
void mic_manager_set_background_held(bool held);
