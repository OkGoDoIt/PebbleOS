/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/cron/cron.h>
#include <pbl/kernel/compiler.h>
#include <pbl/services/notifications/alerts_preferences.h>
#include <pbl/util/units.h>

#include <kernel/events.h>

/**
 * @defgroup services_notifications_do_not_disturb Do Not Disturb
 * @ingroup services_notifications
 * @brief Quiet Time state, combining manual, scheduled and calendar-aware activation.
 *
 * Meant for internal use; clients should ask @ref services_notifications_alerts whether and how
 * the user can be notified. DND is active when any of these modes is:
 *
 * - Manual: an on/off switch. Turning it off while an automatic mode is active overrides that mode
 *   until the next schedule boundary or settings change. It turns itself off when a scheduled
 *   period ends.
 * - Scheduled: up to @ref DND_NUM_SCHEDULE_SLOTS daily periods, each on its own set of days.
 * - Smart (calendar aware): active while a calendar event is ongoing.
 *
 * Every change of the active state posts a @c PEBBLE_DO_NOT_DISTURB_EVENT.
 * @{
 */

/**
 * @brief Daily DND period, in local time.
 *
 * A period whose end is earlier than its start spans midnight; equal start and end never match.
 */
typedef struct PBL_PACKED DoNotDisturbSchedule {
  /** Start hour, 0 to 23. */
  uint8_t from_hour;
  /** Start minute, 0 to 59. */
  uint8_t from_minute;
  /** End hour (exclusive), 0 to 23. */
  uint8_t to_hour;
  /** End minute (exclusive), 0 to 59. */
  uint8_t to_minute;
} DoNotDisturbSchedule;

/** @brief Number of DND schedule slots. */
#define DND_NUM_SCHEDULE_SLOTS (5)

/** @brief Days a DND schedule applies to. */
typedef enum DoNotDisturbScheduleKind {
  /** Every day of the week. */
  DoNotDisturbScheduleKindEveryday,
  /** Monday to Friday. */
  DoNotDisturbScheduleKindWeekdays,
  /** Saturday and Sunday. */
  DoNotDisturbScheduleKindWeekends,
  /** The days set in the slot. */
  DoNotDisturbScheduleKindCustom,
  /** Number of kinds. */
  DoNotDisturbScheduleKindCount,
} DoNotDisturbScheduleKind;

/**
 * @brief DND schedule slot, as stored and synced with the phone.
 *
 * A period spanning midnight belongs to the day it starts on, unless the slot is a legacy one.
 */
typedef struct PBL_PACKED DoNotDisturbScheduleSlot {
  /** Non-zero if the slot holds a schedule. */
  uint8_t used;
  /** Non-zero if the schedule is enabled. */
  uint8_t enabled;
  /** A @ref DoNotDisturbScheduleKind. */
  uint8_t kind;
  /** Days for @ref DoNotDisturbScheduleKindCustom, a PBL_CRON_WDAY_ mask (Sunday is bit 0). */
  uint8_t days;
  /** Daily period. */
  DoNotDisturbSchedule schedule;
  /**
   * Non-zero if the slot holds a weekday or weekend schedule from older firmware: every scheduled
   * day is quiet before the period's end and after its start. Cleared when the days change.
   */
  uint8_t legacy;
} DoNotDisturbScheduleSlot;

/** @brief Where manual DND was toggled from, matching the FirstUseSource values. */
typedef enum ManualDNDFirstUseSource {
  /** Notification action menu. */
  ManualDNDFirstUseSourceActionMenu = 0,
  /** Settings menu. */
  ManualDNDFirstUseSourceSettingsMenu
} ManualDNDFirstUseSource;

/**
 * @brief Check whether DND is in effect.
 *
 * @return true if manual, scheduled or smart DND is active.
 */
bool do_not_disturb_is_active(void);

/**
 * @brief Check whether manual DND is on.
 *
 * @return true if DND has been manually enabled.
 */
bool do_not_disturb_is_manually_enabled(void);

/**
 * @brief Set the manual DND state.
 *
 * Turning it off while scheduled or smart DND is active overrides the automatic mode.
 *
 * @param enable true to turn manual DND on.
 */
void do_not_disturb_set_manually_enabled(bool enable);

#ifdef CONFIG_SERVICE_AUDIO_COMPANION
/** Set the connection-scoped phone Focus contribution without changing stored preferences. */
void do_not_disturb_set_phone_focus(bool enabled);
/** Active source bits: effective, manual, scheduled, calendar, phone Focus, override. */
uint8_t do_not_disturb_source_bits(void);
/** Changes on every manual action, including setting the current value again. */
uint32_t do_not_disturb_manual_revision(void);
#endif

/**
 * @brief Toggle manual DND, showing the first use dialog the first time.
 *
 * From the settings menu this flips the manual setting. From a notification action menu it sets
 * manual DND to the opposite of the current DND active state, showing a confirmation.
 *
 * @param source Where the toggle came from.
 */
void do_not_disturb_toggle_manually_enabled(ManualDNDFirstUseSource source);

/**
 * @brief Check whether calendar-aware (smart) DND is enabled.
 *
 * @return true if enabled.
 */
bool do_not_disturb_is_smart_dnd_enabled(void);

/** @brief Toggle smart DND, showing the first use dialog the first time. */
void do_not_disturb_toggle_smart_dnd(void);

/**
 * @brief Get the days a schedule slot applies to.
 *
 * @param slot Slot.
 * @return PBL_CRON_WDAY_ mask, 0 if the kind is invalid.
 */
static inline uint8_t do_not_disturb_schedule_slot_get_days(const DoNotDisturbScheduleSlot *slot) {
  switch (slot->kind) {
    case DoNotDisturbScheduleKindEveryday:
      return PBL_CRON_WDAY_ANY;
    case DoNotDisturbScheduleKindWeekdays:
      return PBL_CRON_WDAY_WEEKDAYS;
    case DoNotDisturbScheduleKindWeekends:
      return PBL_CRON_WDAY_WEEKENDS;
    case DoNotDisturbScheduleKindCustom:
      return slot->days & PBL_CRON_WDAY_ANY;
    default:
      return 0;
  }
}

/**
 * @brief Check whether a schedule slot holds a valid schedule.
 *
 * @param slot Slot to check.
 * @return true if the kind, period and days are valid.
 */
static inline bool do_not_disturb_schedule_slot_is_valid(const DoNotDisturbScheduleSlot *slot) {
  const DoNotDisturbSchedule *schedule = &slot->schedule;
  if (slot->kind >= DoNotDisturbScheduleKindCount || schedule->from_hour >= PBL_HOUR_PER_DAY ||
      schedule->to_hour >= PBL_HOUR_PER_DAY || schedule->from_minute >= PBL_MIN_PER_HOUR ||
      schedule->to_minute >= PBL_MIN_PER_HOUR) {
    return false;
  }
  if (slot->kind == DoNotDisturbScheduleKindCustom) {
    return slot->days != 0 && (slot->days & ~PBL_CRON_WDAY_ANY) == 0;
  }
  return true;
}

/**
 * @brief Get a DND schedule slot.
 *
 * @param index Slot index, below @ref DND_NUM_SCHEDULE_SLOTS.
 * @param[out] slot Slot, zeroed if unused.
 */
void do_not_disturb_get_schedule_slot(unsigned int index, DoNotDisturbScheduleSlot *slot);

/**
 * @brief Store a DND schedule slot and re-evaluate the DND state.
 *
 * @param index Slot index.
 * @param slot Schedule, marked used when stored.
 * @return 0 on success, -EINVAL if @p index or @p slot is invalid.
 */
int do_not_disturb_set_schedule_slot(unsigned int index, const DoNotDisturbScheduleSlot *slot);

/**
 * @brief Store a DND schedule in the first unused slot and re-evaluate the DND state.
 *
 * @param slot Schedule, marked used when stored.
 * @return Slot index, -EINVAL if @p slot is invalid or -ENOSPC if every slot is used.
 */
int do_not_disturb_add_schedule_slot(const DoNotDisturbScheduleSlot *slot);

/**
 * @brief Clear a DND schedule slot and re-evaluate the DND state.
 *
 * @param index Slot index.
 */
void do_not_disturb_delete_schedule_slot(unsigned int index);

/** @brief Initialize the DND service and arm the schedule timers. */
void do_not_disturb_init(void);

/** @brief Re-evaluate the schedule after the wall clock or timezone changed. */
void do_not_disturb_handle_clock_change(void);

/**
 * @brief Handle a DND state preference (manual, smart or schedule) written by phone sync.
 *
 * Re-evaluates the DND state so the change fires the usual event and updates the schedule timers.
 */
void do_not_disturb_handle_pref_synced(void);

/**
 * @brief Re-evaluate smart DND after a calendar event.
 *
 * @param e Calendar event.
 */
void do_not_disturb_handle_calendar_event(PebbleCalendarEvent *e);

/** @brief Push the manual DND toggle prompt, which sets manual DND to the opposite state. */
void do_not_disturb_manual_toggle_with_dialog(void);

/** @} */
