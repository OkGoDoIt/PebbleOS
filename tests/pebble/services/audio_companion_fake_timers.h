/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// The audio companion's timers are kernel delayed work and its deferrals are background work queue
// calls. These keep the suites' timer vocabulary on top of the kernel fakes.

#include "fake_sys_work.h"
#include "fake_work.h"

typedef struct pbl_delayed_work *TimerID;
#define TIMER_INVALID_ID ((TimerID) nullptr)

static inline bool stub_new_timer_is_scheduled(TimerID timer) {
  return timer != TIMER_INVALID_ID && fake_work_is_scheduled(timer);
}

static inline uint32_t stub_new_timer_timeout(TimerID timer) {
  return fake_work_timeout_ms(timer);
}

//! Runs a scheduled timer's function; false if it was not scheduled.
static inline bool stub_new_timer_fire(TimerID timer) {
  if (!stub_new_timer_is_scheduled(timer)) {
    return false;
  }
  fake_work_fire(timer);
  return true;
}

static inline TimerID stub_new_timer_get_next(void) {
  return fake_work_get_next();
}

//! Fires the soonest timer, num_to_invoke times.
static inline void stub_new_timer_invoke(int num_to_invoke) {
  TimerID timer = stub_new_timer_get_next();
  while (timer != TIMER_INVALID_ID && num_to_invoke--) {
    stub_new_timer_fire(timer);
    timer = stub_new_timer_get_next();
  }
}

static inline void stub_new_timer_cleanup(void) {
  fake_work_cleanup();
}

#define fake_system_task_callbacks_invoke_pending fake_sys_work_callbacks_invoke_pending
#define fake_system_task_callbacks_invoke         fake_sys_work_callbacks_invoke
#define fake_system_task_callbacks_cleanup        fake_sys_work_callbacks_cleanup
#define fake_system_task_count_callbacks          fake_sys_work_count_callbacks
#define fake_system_task_set_available_space      fake_sys_work_set_num_free
