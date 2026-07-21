/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "syscall/syscall.h"

bool alarm_get_next_enabled_alarm(time_t *timestamp_out) {
  (void)timestamp_out;
  return false;
}

void test_music_service_syscalls_disabled__initialize(void) {}

void test_music_service_syscalls_disabled__cleanup(void) {}

void test_music_service_syscalls_disabled__returns_empty_state(void) {
  char title[MUSIC_SERVICE_BUFFER_LENGTH] = "title";
  char artist[MUSIC_SERVICE_BUFFER_LENGTH] = "artist";
  char album[MUSIC_SERVICE_BUFFER_LENGTH] = "album";
  char player_name[MUSIC_SERVICE_BUFFER_LENGTH] = "player";

  cl_assert(!sys_music_has_now_playing());
  sys_music_get_now_playing(title, artist, album);
  cl_assert_equal_s(title, "");
  cl_assert_equal_s(artist, "");
  cl_assert_equal_s(album, "");
  cl_assert_equal_i(sys_music_get_playback_state(), MusicPlayStateUnknown);
  cl_assert(!sys_music_get_player_name(player_name));
  cl_assert_equal_s(player_name, "");

  MusicServicePlaybackInfo info = {
      .position_ms = 1,
      .duration_ms = 2,
      .playback_rate_percent = 100,
      .playback_state = MusicServicePlaybackStatePlaying,
      .capabilities = MusicServiceCapabilityPlaybackState,
      .volume_percent = 50,
  };
  sys_music_get_playback_info(&info);
  cl_assert_equal_i(info.position_ms, 0);
  cl_assert_equal_i(info.duration_ms, 0);
  cl_assert_equal_i(info.playback_rate_percent, 0);
  cl_assert_equal_i(info.playback_state, MusicServicePlaybackStateUnknown);
  cl_assert_equal_i(info.capabilities, 0);
  cl_assert_equal_i(info.volume_percent, 0);
}

void test_music_service_syscalls_disabled__rejects_commands(void) {
  cl_assert(!sys_music_is_command_supported(MusicServiceCommandPlay));
  cl_assert(!sys_music_send_command(MusicServiceCommandPlay));
}
