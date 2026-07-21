/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "pbl/services/music.h"
#include "pbl/util/size.h"
#include "syscall/syscall.h"

static bool s_has_now_playing;
static char s_player_name[MUSIC_SERVICE_BUFFER_LENGTH];
static bool s_has_player_name;
static MusicPlayState s_playback_state;
static int32_t s_playback_rate_percent;
static uint32_t s_position_ms;
static uint32_t s_duration_ms;
static uint8_t s_volume_percent;
static bool s_state_supported;
static bool s_progress_supported;
static bool s_volume_supported;
static MusicCommand s_supported_command;
static MusicCommand s_sent_command;
static unsigned int s_send_count;

bool alarm_get_next_enabled_alarm(time_t *timestamp_out) {
  (void)timestamp_out;
  return false;
}

bool music_has_now_playing(void) {
  return s_has_now_playing;
}

void music_get_now_playing(char *title, char *artist, char *album) {
  strncpy(title, "Track", MUSIC_SERVICE_BUFFER_LENGTH);
  strncpy(artist, "Artist", MUSIC_SERVICE_BUFFER_LENGTH);
  strncpy(album, "Album", MUSIC_SERVICE_BUFFER_LENGTH);
}

MusicPlayState music_get_playback_state(void) {
  return s_playback_state;
}

bool music_get_player_name(char *player_name_out) {
  strncpy(player_name_out, s_player_name, MUSIC_SERVICE_BUFFER_LENGTH);
  return s_has_player_name;
}

bool music_is_playback_state_reporting_supported(void) {
  return s_state_supported;
}

int32_t music_get_playback_rate_percent(void) {
  return s_playback_rate_percent;
}

bool music_is_progress_reporting_supported(void) {
  return s_progress_supported;
}

void music_get_pos(uint32_t *track_pos_ms, uint32_t *track_length_ms) {
  *track_pos_ms = s_position_ms;
  *track_length_ms = s_duration_ms;
}

bool music_is_volume_reporting_supported(void) {
  return s_volume_supported;
}

uint8_t music_get_volume_percent(void) {
  return s_volume_percent;
}

bool music_is_command_supported(MusicCommand command) {
  return command == s_supported_command;
}

void music_command_send(MusicCommand command) {
  s_sent_command = command;
  s_send_count++;
}

void test_music_service_syscalls__initialize(void) {
  s_has_now_playing = false;
  s_player_name[0] = '\0';
  s_has_player_name = false;
  s_playback_state = MusicPlayStateUnknown;
  s_playback_rate_percent = 0;
  s_position_ms = 0;
  s_duration_ms = 0;
  s_volume_percent = 0;
  s_state_supported = false;
  s_progress_supported = false;
  s_volume_supported = false;
  s_supported_command = NumMusicCommand;
  s_sent_command = NumMusicCommand;
  s_send_count = 0;
}

void test_music_service_syscalls__cleanup(void) {}

void test_music_service_syscalls__returns_metadata(void) {
  s_has_now_playing = true;
  s_has_player_name = true;
  strncpy(s_player_name, "Player", sizeof(s_player_name));

  char title[MUSIC_SERVICE_BUFFER_LENGTH];
  char artist[MUSIC_SERVICE_BUFFER_LENGTH];
  char album[MUSIC_SERVICE_BUFFER_LENGTH];
  char player_name[MUSIC_SERVICE_BUFFER_LENGTH];

  cl_assert(sys_music_has_now_playing());
  sys_music_get_now_playing(title, artist, album);
  cl_assert_equal_s(title, "Track");
  cl_assert_equal_s(artist, "Artist");
  cl_assert_equal_s(album, "Album");
  cl_assert(sys_music_get_player_name(player_name));
  cl_assert_equal_s(player_name, "Player");
}

void test_music_service_syscalls__reports_only_supported_playback_fields(void) {
  s_state_supported = true;
  s_playback_state = MusicPlayStatePlaying;
  s_playback_rate_percent = 100;
  s_progress_supported = true;
  s_position_ms = 1234;
  s_duration_ms = 5678;

  MusicServicePlaybackInfo info;
  sys_music_get_playback_info(&info);

  cl_assert_equal_i(info.capabilities,
                    MusicServiceCapabilityPlaybackState | MusicServiceCapabilityProgress);
  cl_assert_equal_i(info.playback_state, MusicServicePlaybackStatePlaying);
  cl_assert_equal_i(info.playback_rate_percent, 100);
  cl_assert_equal_i(info.position_ms, 1234);
  cl_assert_equal_i(info.duration_ms, 5678);
  cl_assert_equal_i(info.volume_percent, 0);
}

void test_music_service_syscalls__reports_volume(void) {
  s_volume_supported = true;
  s_volume_percent = 42;

  MusicServicePlaybackInfo info;
  sys_music_get_playback_info(&info);

  cl_assert_equal_i(info.capabilities, MusicServiceCapabilityVolume);
  cl_assert_equal_i(info.volume_percent, 42);
  cl_assert_equal_i(info.playback_state, MusicServicePlaybackStateUnknown);
}

void test_music_service_syscalls__maps_every_playback_state(void) {
  const struct {
    MusicPlayState private_state;
    MusicServicePlaybackState public_state;
  } cases[] = {
      {MusicPlayStateUnknown, MusicServicePlaybackStateUnknown},
      {MusicPlayStatePlaying, MusicServicePlaybackStatePlaying},
      {MusicPlayStatePaused, MusicServicePlaybackStatePaused},
      {MusicPlayStateForwarding, MusicServicePlaybackStateForwarding},
      {MusicPlayStateRewinding, MusicServicePlaybackStateRewinding},
      {MusicPlayStateInvalid, MusicServicePlaybackStateUnknown},
  };

  s_state_supported = true;
  for (size_t i = 0; i < ARRAY_LENGTH(cases); i++) {
    s_playback_state = cases[i].private_state;

    MusicServicePlaybackInfo info;
    sys_music_get_playback_info(&info);

    cl_assert_equal_i(info.playback_state, cases[i].public_state);
  }
}

void test_music_service_syscalls__maps_every_supported_command(void) {
  const struct {
    MusicServiceCommand public_command;
    MusicCommand private_command;
  } cases[] = {
      {MusicServiceCommandPlay, MusicCommandPlay},
      {MusicServiceCommandPause, MusicCommandPause},
      {MusicServiceCommandTogglePlayPause, MusicCommandTogglePlayPause},
      {MusicServiceCommandNextTrack, MusicCommandNextTrack},
      {MusicServiceCommandPreviousTrack, MusicCommandPreviousTrack},
      {MusicServiceCommandVolumeUp, MusicCommandVolumeUp},
      {MusicServiceCommandVolumeDown, MusicCommandVolumeDown},
      {MusicServiceCommandAdvanceRepeatMode, MusicCommandAdvanceRepeatMode},
      {MusicServiceCommandAdvanceShuffleMode, MusicCommandAdvanceShuffleMode},
      {MusicServiceCommandSkipForward, MusicCommandSkipForward},
      {MusicServiceCommandSkipBackward, MusicCommandSkipBackward},
      {MusicServiceCommandLike, MusicCommandLike},
      {MusicServiceCommandDislike, MusicCommandDislike},
      {MusicServiceCommandBookmark, MusicCommandBookmark},
  };

  for (size_t i = 0; i < ARRAY_LENGTH(cases); i++) {
    s_supported_command = cases[i].private_command;
    cl_assert(sys_music_is_command_supported(cases[i].public_command));
    cl_assert(sys_music_send_command(cases[i].public_command));
    cl_assert_equal_i(s_sent_command, cases[i].private_command);
    cl_assert_equal_i(s_send_count, i + 1);
  }
}

void test_music_service_syscalls__rejects_invalid_or_unsupported_commands(void) {
  const MusicServiceCommand invalid_command =
      (MusicServiceCommand)(MusicServiceCommandBookmark + 1);

  cl_assert(!sys_music_is_command_supported(invalid_command));
  cl_assert(!sys_music_send_command(invalid_command));
  cl_assert_equal_i(s_send_count, 0);

  s_supported_command = MusicCommandPause;
  cl_assert(!sys_music_send_command(MusicServiceCommandPlay));
  cl_assert_equal_i(s_send_count, 0);
}
