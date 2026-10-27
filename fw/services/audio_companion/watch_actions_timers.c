/* SPDX-License-Identifier: Apache-2.0 */

#include "watch_actions.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <kernel/pbl_malloc.h>
#include <pbl/drivers/rtc.h>
#include <pbl/services/persist.h>
#include <pbl/services/settings/settings_file.h>
#include <pbl/services/wakeup.h>
#include <process_management/app_install_manager.h>
#include <process_management/app_manager.h>

// Saved format verified against Core Devices pebble-timer v1.2.7.
// Never read or write while that app owns a live in-memory copy.
static const Uuid s_timer_uuid = UuidMake(0xc6, 0x4a, 0xae, 0x42, 0xb8, 0x22, 0x4d, 0x6d,
                                         0x99, 0x5b, 0xbc, 0x7c, 0x38, 0xe8, 0x43, 0xf4);
#define TIMER_COUNT_KEY 72445846u
#define TIMER_VERSION_KEY 46134672u
#define TIMER_MUSE_ID_KEY 0x4d555345u
#define TIMER_MAX 8
#define TIMER_EXPIRED INT64_MIN

typedef struct __attribute__((packed)) {
  int64_t start_ms;
  int64_t duration_ms;
  int32_t id;
  char display_buffer[16];
  uint8_t paused;
  int32_t last_update;
} NativeTimer;
static_assert(sizeof(NativeTimer) == 41, "Core Devices Timer record layout");

typedef struct {
  NativeTimer timers[TIMER_MAX];
  int32_t count;
  int32_t last_muse_id;
  SettingsFile *destination;
  status_t status;
} TimerStore;

static bool prv_read(SettingsFile *file, uint32_t key, void *value, size_t size) {
  return settings_file_get_len(file, &key, sizeof(key)) == (int)size &&
         settings_file_get(file, &key, sizeof(key), value, size) >= 0;
}

static int64_t prv_remaining(const NativeTimer *timer, int64_t now) {
  if (timer->start_ms == TIMER_EXPIRED) {
    return 0;
  }
  const int64_t remaining = timer->duration_ms + timer->start_ms - (timer->paused ? 0 : now);
  return remaining < 0 ? 0 : (remaining > timer->duration_ms ? timer->duration_ms : remaining);
}

static bool prv_load(SettingsFile *file, TimerStore *store) {
  uint32_t key = TIMER_MUSE_ID_KEY;
  if (settings_file_exists(file, &key, sizeof(key)) &&
      (!prv_read(file, key, &store->last_muse_id, sizeof(store->last_muse_id)) ||
       store->last_muse_id < 0 || store->last_muse_id >= 0x40000000)) {
    return false;
  }
  key = TIMER_COUNT_KEY;
  if (!settings_file_exists(file, &key, sizeof(key))) {
    // A fresh app has no timers yet. A foreign version must never be overwritten.
    key = TIMER_VERSION_KEY;
    int32_t version;
    return !settings_file_exists(file, &key, sizeof(key)) ||
           (prv_read(file, key, &version, sizeof(version)) && version == 1);
  }
  int32_t version;
  if (!prv_read(file, TIMER_VERSION_KEY, &version, sizeof(version)) || version != 1 ||
      !prv_read(file, TIMER_COUNT_KEY, &store->count, sizeof(store->count)) ||
      store->count < 0 || store->count > TIMER_MAX) {
    return false;
  }
  for (int i = 0; i < store->count; ++i) {
    NativeTimer *timer = &store->timers[i];
    if (!prv_read(file, TIMER_COUNT_KEY + 1 + i, timer, sizeof(*timer)) ||
        timer->id <= 0 || timer->paused > 1 || timer->duration_ms <= 0 ||
        timer->duration_ms > INT32_MAX ||
        (timer->start_ms == TIMER_EXPIRED ? !timer->paused :
         (timer->paused ? (timer->start_ms > 0 || timer->start_ms < -timer->duration_ms) :
          (timer->start_ms < 0 || timer->start_ms > (int64_t)INT32_MAX * 1000)))) {
      return false;
    }
    for (int j = 0; j < i; ++j) {
      if (store->timers[j].id == timer->id) {
        return false;
      }
    }
  }
  return true;
}

static bool prv_copy_other_records(SettingsFile *file, SettingsRecordInfo *info, void *data) {
  TimerStore *store = data;
  uint32_t key = 0;
  if (info->key_len == sizeof(key)) {
    info->get_key(file, &key, sizeof(key));
    if (key == TIMER_VERSION_KEY || key == TIMER_MUSE_ID_KEY ||
        (key >= TIMER_COUNT_KEY && key <= TIMER_COUNT_KEY + TIMER_MAX)) {
      return true;
    }
  }
  uint8_t *bytes = kernel_malloc(info->key_len + info->val_len);
  if (!bytes) {
    store->status = E_OUT_OF_MEMORY;
    return false;
  }
  info->get_key(file, bytes, info->key_len);
  info->get_val(file, bytes + info->key_len, info->val_len);
  store->status = settings_file_set(store->destination, bytes, info->key_len,
                                    bytes + info->key_len, info->val_len);
  kernel_free(bytes);
  return store->status >= 0;
}

static status_t prv_save(SettingsFile *old, SettingsFile *replacement, void *data) {
  TimerStore *store = data;
  store->destination = replacement;
  settings_file_each(old, prv_copy_other_records, store);
  if (store->status < 0) {
    return store->status;
  }
  uint32_t key = TIMER_VERSION_KEY;
  int32_t version = 1;
  status_t status = settings_file_set(replacement, &key, sizeof(key), &version, sizeof(version));
  key = TIMER_MUSE_ID_KEY;
  if (status >= 0) {
    status = settings_file_set(replacement, &key, sizeof(key), &store->last_muse_id,
                               sizeof(store->last_muse_id));
  }
  key = TIMER_COUNT_KEY;
  if (status >= 0) {
    status = settings_file_set(replacement, &key, sizeof(key), &store->count, sizeof(store->count));
  }
  for (int i = 0; status >= 0 && i < store->count; ++i) {
    key = TIMER_COUNT_KEY + 1 + i;
    status = settings_file_set(replacement, &key, sizeof(key), &store->timers[i], sizeof(NativeTimer));
  }
  return status;
}

static time_t prv_deadline(const TimerStore *store, int64_t now) {
  int64_t earliest = INT64_MAX;
  for (int i = 0; i < store->count; ++i) {
    const NativeTimer *timer = &store->timers[i];
    if (!timer->paused) {
      int64_t end = now + prv_remaining(timer, now);
      if (end < earliest) {
        earliest = end;
      }
    }
  }
  // Native Timer schedules one second past expiry so its own alert logic sees it due.
  return earliest == INT64_MAX ? 0 : (time_t)(earliest / 1000 + 1);
}

bool watch_actions_native_timers(const WatchActionRequest *request, char *text, size_t size) {
  const AppInstallId installed = app_install_get_id_for_uuid(&s_timer_uuid);
  if (installed == INSTALL_ID_INVALID) {
    snprintf(text, size, "not_installed: Install Timer by Pebble/Core Devices on the watch.");
    return false;
  }
  const PebbleProcessMd *current = app_manager_get_current_app_md();
  if ((current && uuid_equal(&current->uuid, &s_timer_uuid)) ||
      (app_manager_get_current_worker() &&
       uuid_equal(&app_manager_get_current_worker()->uuid, &s_timer_uuid))) {
    snprintf(text, size, "busy: Close Timer before managing its timers with Muse.");
    return false;
  }
  if (request->timers.label_present && request->timers.label[0]) {
    snprintf(text, size, "unsupported_label: The native Timer app has no timer labels.");
    return false;
  }
  persist_service_client_open(&s_timer_uuid);
  SettingsFile *file = persist_service_lock_and_get_store(&s_timer_uuid);
  TimerStore store = {0};
  bool ok = false;
  if (!file || !prv_load(file, &store)) {
    snprintf(text, size, "incompatible_data: Timer data could not be read safely; nothing changed.");
    goto done;
  }
  time_t seconds;
  uint16_t milliseconds;
  rtc_get_time_ms(&seconds, &milliseconds);
  const int64_t now = (int64_t)seconds * 1000 + milliseconds;
  const time_t old_deadline = prv_deadline(&store, now);
  const uint8_t operation = request->timers.operation;
  NativeTimer *timer = NULL;
  int index = -1;
  for (int i = 0; i < store.count; ++i) {
    if ((uint32_t)store.timers[i].id == request->timers.id) {
      timer = &store.timers[i];
      index = i;
    }
  }
  if (operation == 0) {
    size_t used = 0;
    for (int i = 0; i < store.count; ++i) {
      used += snprintf(text + used, size - used, "%" PRId32 "\n", store.timers[i].id);
    }
    ok = true;
    goto done;
  }
  if (operation == 1) {
    if (store.count == TIMER_MAX) {
      snprintf(text, size, "full: Timer already contains eight timers; delete one first.");
      goto done;
    }
    // Native UI uses current Unix seconds for new ids. Keep Muse's durable sequence below
    // that range, so rapid creations cannot collide with a subsequent on-watch creation.
    int32_t id = store.last_muse_id;
    bool collision;
    do {
      if (++id >= 0x40000000) {
        snprintf(text, size, "id_exhausted: No timer was created.");
        goto done;
      }
      collision = false;
      for (int i = 0; i < store.count; ++i) {
        collision |= store.timers[i].id == id;
      }
    } while (collision);
    store.last_muse_id = id;
    timer = &store.timers[store.count++];
    *timer = (NativeTimer){.id = id, .start_ms = now,
                           .duration_ms = (int64_t)request->timers.seconds * 1000};
  } else if (!timer) {
    snprintf(text, size, "not_found: List timers again; native app edits can change their ids.");
    goto done;
  } else if (operation == 3 || operation == 4) {
    int64_t remaining = prv_remaining(timer, now);
    if (!remaining) {
      snprintf(text, size, "finished: Use restart to start this timer again.");
      goto done;
    }
    timer->paused = operation == 3;
    timer->start_ms = remaining - timer->duration_ms + (timer->paused ? 0 : now);
  } else if (operation == 5 && request->timers.seconds) {
    timer->duration_ms = (int64_t)request->timers.seconds * 1000;
    timer->start_ms = timer->paused ? 0 : now;
  } else if (operation == 6) {
    timer->start_ms = now;
    timer->paused = false;
  } else if (operation == 7) {
    memmove(&store.timers[index], &store.timers[index + 1],
            (store.count - index - 1) * sizeof(NativeTimer));
    store.count--;
    timer = NULL;
  }
  if (operation != 2) {
    if (timer) {
      timer->last_update = seconds;
    }
    const time_t next = prv_deadline(&store, now);
    const time_t guard_deadline = old_deadline && next ?
        (old_deadline < next ? old_deadline : next) : (old_deadline ? old_deadline : next);
    // Arm before committing. A reset between files may open Timer early, but never leaves a
    // committed running timer without a wakeup. Timer reads the canonical records on launch.
    if (wakeup_replace_app_deadline(&s_timer_uuid, guard_deadline) < 0 ||
        settings_file_transaction(file, prv_save, &store) < 0) {
      snprintf(text, size, "storage_error: Change not confirmed; read timers before retrying.");
      goto done;
    }
    // An earlier guard left by a failed cleanup is harmless: Timer will re-arm its next expiry.
    wakeup_replace_app_deadline(&s_timer_uuid, next);
  }
  if (timer) {
    const int64_t remaining = prv_remaining(timer, now);
    snprintf(text, size, "%" PRId32 ",%u,%" PRId64 ",%" PRId64 "\n", timer->id,
             remaining == 0 ? 3 : (timer->paused ? 2 : 1), remaining,
             (timer->duration_ms + 999) / 1000);
  }
  ok = true;
done:
  persist_service_unlock_store(file);
  persist_service_client_close(&s_timer_uuid);
  return ok;
}
