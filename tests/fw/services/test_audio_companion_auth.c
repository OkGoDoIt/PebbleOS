/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "pbl/services/filesystem/pfs.h"

#include "services/audio_companion/auth.h"

#include "fake_spi_flash.h"
#include "fake_rtc.h"

#include "stubs_analytics.h"
#include "stubs_hexdump.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_pebble_tasks.h"
#include "stubs_prompt.h"
#include "stubs_rand_ptr.h"
#include "stubs_serial.h"
#include "stubs_sleep.h"
#include "stubs_task_watchdog.h"

#include <string.h>

static const uint8_t s_receiver_a[AUDIO_COMPANION_RECEIVER_ID_BYTES] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
  0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
  0x1e, 0x1f,
};

static const uint8_t s_receiver_b[AUDIO_COMPANION_RECEIVER_ID_BYTES] = {
  0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
  0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
  0x1e, 0x1f,
};

void test_audio_companion_auth__initialize(void) {
  fake_spi_flash_init(0, 0x1000000);
  pfs_init(false);
  audio_companion_auth_init();
}

void test_audio_companion_auth__cleanup(void) {}

void test_audio_companion_auth__empty_slot_by_default(void) {
  cl_assert(!audio_companion_auth_receiver_exists());
  cl_assert_equal_i(audio_companion_auth_evaluate(s_receiver_a),
                    AudioCompanionAuthEvalNoReceiver);
  char name[32];
  cl_assert(!audio_companion_auth_get_receiver_name(name, sizeof(name)));
}

void test_audio_companion_auth__bind_then_match(void) {
  cl_assert(audio_companion_auth_store_receiver(s_receiver_a, "Audio Companion"));
  cl_assert(audio_companion_auth_receiver_exists());
  cl_assert_equal_i(audio_companion_auth_evaluate(s_receiver_a), AudioCompanionAuthEvalMatch);

  char name[32];
  cl_assert(audio_companion_auth_get_receiver_name(name, sizeof(name)));
  cl_assert_equal_s(name, "Audio Companion");
}

void test_audio_companion_auth__mismatch_fails_closed(void) {
  cl_assert(audio_companion_auth_store_receiver(s_receiver_a, "Audio Companion"));
  cl_assert_equal_i(audio_companion_auth_evaluate(s_receiver_b),
                    AudioCompanionAuthEvalMismatch);

  // A second bind without revoke must be refused even for the same identity.
  cl_assert(!audio_companion_auth_store_receiver(s_receiver_b, "Impostor"));
  cl_assert(!audio_companion_auth_store_receiver(s_receiver_a, "Audio Companion"));
  cl_assert_equal_i(audio_companion_auth_evaluate(s_receiver_a), AudioCompanionAuthEvalMatch);
}

void test_audio_companion_auth__forget_allows_rebind(void) {
  cl_assert(audio_companion_auth_store_receiver(s_receiver_a, "Audio Companion"));
  audio_companion_auth_forget_receiver();
  cl_assert(!audio_companion_auth_receiver_exists());
  cl_assert_equal_i(audio_companion_auth_evaluate(s_receiver_a),
                    AudioCompanionAuthEvalNoReceiver);

  cl_assert(audio_companion_auth_store_receiver(s_receiver_b, "Replacement"));
  cl_assert_equal_i(audio_companion_auth_evaluate(s_receiver_b), AudioCompanionAuthEvalMatch);
  cl_assert_equal_i(audio_companion_auth_evaluate(s_receiver_a),
                    AudioCompanionAuthEvalMismatch);
}

void test_audio_companion_auth__persists_across_reinit(void) {
  cl_assert(audio_companion_auth_store_receiver(s_receiver_a, "Audio Companion"));

  // Simulate reboot: re-init reloads from the settings file (flash persists).
  audio_companion_auth_init();
  cl_assert(audio_companion_auth_receiver_exists());
  cl_assert_equal_i(audio_companion_auth_evaluate(s_receiver_a), AudioCompanionAuthEvalMatch);
  char name[32];
  cl_assert(audio_companion_auth_get_receiver_name(name, sizeof(name)));
  cl_assert_equal_s(name, "Audio Companion");

  audio_companion_auth_forget_receiver();
  audio_companion_auth_init();
  cl_assert(!audio_companion_auth_receiver_exists());
}

void test_audio_companion_auth__max_length_name_is_terminated(void) {
  cl_assert(audio_companion_auth_store_receiver(s_receiver_a, "xxxxxxxxxxxxxxxxxxxxxxxx"));
  char name[AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES + 1];
  cl_assert(audio_companion_auth_get_receiver_name(name, sizeof(name)));
  cl_assert_equal_i(strlen(name), AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES);
}
