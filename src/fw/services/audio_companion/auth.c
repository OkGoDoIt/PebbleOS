/* SPDX-License-Identifier: Apache-2.0 */

#include "auth.h"

#include "pbl/services/settings/settings_file.h"
#include "drivers/rtc.h"
#include "system/logging.h"
#include "util/attributes.h"

#include "tinycrypt/sha256.h"
#include "tinycrypt/constants.h"

#include <inttypes.h>
#include <string.h>

#define AUTH_SETTINGS_FILE_NAME "audiocomp"
#define AUTH_SETTINGS_FILE_SIZE (1024)
#define AUTH_RECEIVER_KEY "receiver"

typedef struct PACKED {
  uint8_t version;
  uint8_t receiver_hash[TC_SHA256_DIGEST_SIZE];
  char name[AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES + 1];
  uint32_t authorized_at;
} StoredReceiver;

#define STORED_RECEIVER_VERSION (1)

//! RAM cache of the persisted record so evaluate() never blocks on flash.
static StoredReceiver s_receiver;
static bool s_receiver_loaded;

static void prv_hash_receiver_id(const uint8_t *receiver_id, uint8_t *hash_out) {
  struct tc_sha256_state_struct sha;
  tc_sha256_init(&sha);
  tc_sha256_update(&sha, receiver_id, AUDIO_COMPANION_RECEIVER_ID_BYTES);
  tc_sha256_final(hash_out, &sha);
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
  uint8_t hash[TC_SHA256_DIGEST_SIZE];
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
    settings_file_close(&file);
  }
  memset(&s_receiver, 0, sizeof(s_receiver));
  s_receiver_loaded = false;
  PBL_LOG_DBG("Audio receiver forgotten");
}
