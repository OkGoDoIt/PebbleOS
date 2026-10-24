/* SPDX-License-Identifier: Apache-2.0 */

#include "auth.h"

#include "pbl/services/settings/settings_file.h"
#include <pbl/drivers/rtc.h>
#include "pbl/logging/logging.h"
#include <pbl/kernel/compiler.h>
#include "pbl/kernel/assert.h"

#include <mbedtls/sha256.h>

#include <inttypes.h>
#include <string.h>

PBL_LOG_MODULE_DEFINE(service_audio_companion, CONFIG_SERVICE_AUDIO_COMPANION_LOG_LEVEL);

#define AUTH_RECEIVER_HASH_BYTES (32)

#define AUTH_SETTINGS_FILE_NAME "audiocomp"
#define AUTH_SETTINGS_FILE_SIZE (1024)
#define AUTH_RECEIVER_KEY "receiver"
#define AUTH_MARKS_KEY          "marks"
#define AUTH_MARKS_MAX_BYTES    (256)

typedef struct PBL_PACKED {
  uint8_t receiver_hash[AUTH_RECEIVER_HASH_BYTES];
  uint16_t length;
  uint8_t data[AUTH_MARKS_MAX_BYTES];
} StoredMarks;

typedef struct PBL_PACKED {
  uint8_t version;
  uint8_t receiver_hash[AUTH_RECEIVER_HASH_BYTES];
  char name[AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES + 1];
  uint32_t authorized_at;
} StoredReceiver;

#define STORED_RECEIVER_VERSION (1)

//! RAM cache of the persisted record so evaluate() never blocks on flash.
static StoredReceiver s_receiver;
static bool s_receiver_loaded;

static void prv_hash_receiver_id(const uint8_t *receiver_id, uint8_t *hash_out) {
  PBL_ASSERTN(mbedtls_sha256(receiver_id, AUDIO_COMPANION_RECEIVER_ID_BYTES, hash_out,
                             0 /* SHA-256, not SHA-224 */) == 0);
}

static bool prv_load_from_flash(void) {
  SettingsFile file;
  if (settings_file_open(&file, AUTH_SETTINGS_FILE_NAME, AUTH_SETTINGS_FILE_SIZE) != S_SUCCESS) {
    return false;
  }
  StoredReceiver stored;
  const status_t status = settings_file_get(&file, AUTH_RECEIVER_KEY, strlen(AUTH_RECEIVER_KEY),
                                            &stored, sizeof(stored));
  settings_file_close(&file);
  if (status != S_SUCCESS || stored.version != STORED_RECEIVER_VERSION) {
    return false;
  }
  stored.name[AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES] = '\0';
  s_receiver = stored;
  return true;
}

void audio_companion_auth_init(void) {
  memset(&s_receiver, 0, sizeof(s_receiver));
  s_receiver_loaded = prv_load_from_flash();
}

bool audio_companion_auth_receiver_exists(void) { return s_receiver_loaded; }

bool audio_companion_auth_get_receiver_name(char *buf, size_t buf_size) {
  if (!s_receiver_loaded || !buf || buf_size == 0) {
    return false;
  }
  strncpy(buf, s_receiver.name, buf_size - 1);
  buf[buf_size - 1] = '\0';
  return true;
}

AudioCompanionAuthEval audio_companion_auth_evaluate(
    const uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES]) {
  if (!s_receiver_loaded) {
    return AudioCompanionAuthEvalNoReceiver;
  }
  uint8_t hash[AUTH_RECEIVER_HASH_BYTES];
  prv_hash_receiver_id(receiver_id, hash);
  if (memcmp(hash, s_receiver.receiver_hash, sizeof(hash)) == 0) {
    return AudioCompanionAuthEvalMatch;
  }
  return AudioCompanionAuthEvalMismatch;
}

bool audio_companion_auth_store_receiver(
    const uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES], const char *name) {
  if (s_receiver_loaded) {
    PBL_LOG_WRN("Refusing to bind audio receiver: slot occupied");
    return false;
  }

  StoredReceiver stored = {
    .version = STORED_RECEIVER_VERSION,
    .authorized_at = (uint32_t)rtc_get_time(),
  };
  prv_hash_receiver_id(receiver_id, stored.receiver_hash);
  if (name) {
    strncpy(stored.name, name, AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES);
  }

  SettingsFile file;
  if (settings_file_open(&file, AUTH_SETTINGS_FILE_NAME, AUTH_SETTINGS_FILE_SIZE) != S_SUCCESS) {
    return false;
  }
  const status_t status = settings_file_set(&file, AUTH_RECEIVER_KEY, strlen(AUTH_RECEIVER_KEY),
                                            &stored, sizeof(stored));
  settings_file_close(&file);
  if (status != S_SUCCESS) {
    PBL_LOG_ERR("Failed to persist audio receiver: %" PRId32, (int32_t)status);
    return false;
  }

  s_receiver = stored;
  s_receiver_loaded = true;
  PBL_LOG_DBG("Audio receiver bound: %s", stored.name);
  return true;
}

void audio_companion_auth_forget_receiver(void) {
  SettingsFile file;
  if (settings_file_open(&file, AUTH_SETTINGS_FILE_NAME, AUTH_SETTINGS_FILE_SIZE) == S_SUCCESS) {
    settings_file_delete(&file, AUTH_RECEIVER_KEY, strlen(AUTH_RECEIVER_KEY));
    settings_file_delete(&file, AUTH_MARKS_KEY, strlen(AUTH_MARKS_KEY));
    settings_file_close(&file);
  }
  memset(&s_receiver, 0, sizeof(s_receiver));
  s_receiver_loaded = false;
  PBL_LOG_DBG("Audio receiver forgotten");
}

bool audio_companion_auth_load_marks(void *data, size_t length) {
  if (!s_receiver_loaded || length > AUTH_MARKS_MAX_BYTES) {
    return false;
  }
  SettingsFile file;
  if (settings_file_open(&file, AUTH_SETTINGS_FILE_NAME, AUTH_SETTINGS_FILE_SIZE) != S_SUCCESS) {
    return false;
  }
  StoredMarks marks;
  const status_t status =
      settings_file_get(&file, AUTH_MARKS_KEY, strlen(AUTH_MARKS_KEY), &marks, sizeof(marks));
  settings_file_close(&file);
  if (status != S_SUCCESS || marks.length != length ||
      memcmp(marks.receiver_hash, s_receiver.receiver_hash, AUTH_RECEIVER_HASH_BYTES) != 0) {
    return false;
  }
  memcpy(data, marks.data, length);
  return true;
}

bool audio_companion_auth_save_marks(const void *data, size_t length) {
  if (!s_receiver_loaded || length > AUTH_MARKS_MAX_BYTES) {
    return false;
  }
  StoredMarks marks = {.length = length};
  memcpy(marks.receiver_hash, s_receiver.receiver_hash, AUTH_RECEIVER_HASH_BYTES);
  memcpy(marks.data, data, length);
  SettingsFile file;
  if (settings_file_open(&file, AUTH_SETTINGS_FILE_NAME, AUTH_SETTINGS_FILE_SIZE) != S_SUCCESS) {
    return false;
  }
  const status_t status =
      settings_file_set(&file, AUTH_MARKS_KEY, strlen(AUTH_MARKS_KEY), &marks, sizeof(marks));
  settings_file_close(&file);
  return status == S_SUCCESS;
}
