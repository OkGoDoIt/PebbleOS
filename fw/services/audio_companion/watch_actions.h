/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/services/audio_companion_private.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! Watch actions (PROTOCOL.md 4.5): the receiver's assistant asks the watch to set a timer, a
//! one-off alarm, or to put a choice to the wearer. Fragments are assembled here; complete actions
//! run on the system task, outside the service lock; the outcome goes back as
//! WATCH_ACTION_RESULT.

#define WATCH_ACTIONS_MAX_TIMERS (4)

typedef struct {
  uint8_t kind;  // AudioCompanionWatchActionKind
  union {
    struct {
      uint32_t seconds;
      char label[AUDIO_COMPANION_WATCH_ACTION_LABEL_MAX_BYTES + 1];
    } timer;
    struct {
      uint8_t hour;
      uint8_t minute;
      char label[AUDIO_COMPANION_WATCH_ACTION_LABEL_MAX_BYTES + 1];
    } alarm;
    struct {
      uint16_t timeout_s;
      char question[AUDIO_COMPANION_WATCH_ACTION_QUESTION_MAX_BYTES + 1];
      uint8_t count;
      char options[AUDIO_COMPANION_WATCH_ACTION_MAX_OPTIONS]
                  [AUDIO_COMPANION_WATCH_ACTION_OPTION_MAX_BYTES + 1];
    } choose;
    struct {
      uint8_t command;  // AudioCompanionWatchMusicCommand
    } music;
    struct {
      bool on;
      uint16_t minutes;
    } quiet_time;
    struct {
      char name[AUDIO_COMPANION_WATCH_ACTION_APP_NAME_MAX_BYTES + 1];
    } launch_app;
    struct {
      uint8_t seconds;
    } find_watch;
    struct {
      uint8_t metric;  // AudioCompanionWatchHealthMetric
      uint8_t days;
    } health;
    struct {
      char text[AUDIO_COMPANION_WATCH_ACTION_NOTICE_MAX_BYTES + 1];
    } notice;
  };
} WatchActionRequest;

//! Parses a joined payload. False when it doesn't follow 4.5; *status_out then says whether the
//! kind was unknown (Unsupported) or the payload malformed (Failed).
bool watch_actions_parse(const uint8_t *payload, size_t length, WatchActionRequest *out,
                         uint8_t *status_out);

//! Shows a choice and eventually calls watch_actions_choice_result(). Called on KernelMain.
typedef void (*WatchActionsChooseHandler)(uint8_t action_id, const WatchActionRequest *request);
//! A timer ended. Called on KernelMain.
typedef void (*WatchActionsTimerDoneHandler)(const char *label);
//! Takes a choice that is showing off the screen (its receiver went away). KernelMain.
typedef void (*WatchActionsChooseDismissHandler)(void);

void watch_actions_set_ui_handlers(WatchActionsChooseHandler choose,
                                   WatchActionsTimerDoneHandler timer_done,
                                   WatchActionsChooseDismissHandler dismiss);

//! Shows a short message from the receiver. Called on KernelMain; `text` lasts only the call.
typedef void (*WatchActionsNoticeHandler)(const char *text);

void watch_actions_set_notice_handler(WatchActionsNoticeHandler notice);

void watch_actions_init(void);

//! One WATCH_ACTION write, from the control dispatch. Returns the ACK status.
uint8_t watch_actions_handle_fragment(uint8_t action_id, uint8_t flags, const uint8_t *data,
                                      uint8_t length);

//! The wearer answered (or didn't). Any task.
void watch_actions_choice_result(uint8_t action_id, uint8_t status, uint8_t index);

//! The receiver is gone: a half-assembled action is dropped and a choice on screen is taken away.
//! Timers and alarms already set stay.
void watch_actions_receiver_gone(void);

//! Timers still counting, for tests and diagnostics.
uint32_t watch_actions_active_timer_count(void);

//! Provided by audio_companion.c: sends WATCH_ACTION_RESULT to the attached receiver. `text` may be
//! NULL.
bool audio_companion_send_watch_action_result(uint8_t action_id, uint8_t status, uint8_t value,
                                              const char *text);

// ---- Provided by the platform (watch_actions_platform.c); seams for tests ----

//! A one-off alarm in the watch's alarms. System task.
bool watch_actions_create_alarm(uint8_t hour, uint8_t minute);

//! Sends `command` to the phone's music player through the watch's link, and writes what is
//! playing ("title\nartist") into `text`. False when no player is connected. System task.
bool watch_actions_music(uint8_t command, char *text, size_t text_size);

//! Quiet Time on or off. System task.
void watch_actions_set_quiet_time(bool on);

//! Opens the watch app whose name matches `name` and writes its name into `found`. False when no
//! app matches. System task.
bool watch_actions_launch_app(const char *name, char *found, size_t found_size);

//! Buzzes and lights the watch for `seconds`. System task.
void watch_actions_find_watch(uint8_t seconds);

//! The watch's activity data for `metric` over `days` (today first), comma-separated, into
//! `text`. False when the activity service has nothing; `text` may then say why
//! (AUDIO_COMPANION_WATCH_HEALTH_HEART_RATE_OFF). System task.
bool watch_actions_health(uint8_t metric, uint8_t days, char *text, size_t text_size);

//! Each of the last `days` days' heart rate (today first) from the minute history, read a slice
//! at a time on the system task so nothing else waits; sends action `action_id`'s result when
//! done. System task.
void watch_actions_heart_rate_daily(uint8_t action_id, uint8_t days);

//! Unit tests only.
void watch_actions_test_reset(void);
