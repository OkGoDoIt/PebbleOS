/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "applib/music_service_private.h"
#include "kernel/events.h"
#include "pbl/services/music.h"
#include "pbl/util/size.h"

#include "stubs_passert.h"
#include "fake_event_service.h"

static MusicServiceState s_music_service_state;
static MusicServiceEventType s_last_event;
static unsigned int s_event_count;

static bool s_has_now_playing;
static char s_title[MUSIC_SERVICE_BUFFER_LENGTH];
static char s_artist[MUSIC_SERVICE_BUFFER_LENGTH];
static char s_album[MUSIC_SERVICE_BUFFER_LENGTH];
static MusicPlayState s_playback_state;
static bool s_has_player_name;
static char s_player_name[MUSIC_SERVICE_BUFFER_LENGTH];
static MusicServicePlaybackInfo s_playback_info;
static MusicServiceCommand s_last_command;
static bool s_command_supported;

MusicServiceState *app_state_get_music_service_state(void) {
  return &s_music_service_state;
}

MusicServiceState *worker_state_get_music_service_state(void) {
  return &s_music_service_state;
}

PebbleTask pebble_task_get_current(void) {
  return PebbleTask_App;
}

void kernel_free(void *ptr) {
  (void)ptr;
}

bool sys_music_has_now_playing(void) {
  return s_has_now_playing;
}

void sys_music_get_now_playing(char *title, char *artist, char *album) {
  strncpy(title, s_title, MUSIC_SERVICE_BUFFER_LENGTH);
  strncpy(artist, s_artist, MUSIC_SERVICE_BUFFER_LENGTH);
  strncpy(album, s_album, MUSIC_SERVICE_BUFFER_LENGTH);
}

MusicPlayState sys_music_get_playback_state(void) {
  return s_playback_state;
}

bool sys_music_get_player_name(char *player_name) {
  strncpy(player_name, s_player_name, MUSIC_SERVICE_BUFFER_LENGTH);
  return s_has_player_name;
}

void sys_music_get_playback_info(MusicServicePlaybackInfo *playback_info) {
  *playback_info = s_playback_info;
}

bool sys_music_is_command_supported(MusicServiceCommand command) {
  s_last_command = command;
  return s_command_supported;
}

bool sys_music_send_command(MusicServiceCommand command) {
  s_last_command = command;
  return s_command_supported;
}

static void prv_event_handler(MusicServiceEventType event) {
  s_last_event = event;
  s_event_count++;
}

void test_music_service__initialize(void) {
  s_music_service_state = (MusicServiceState){};
  s_last_event = MusicServiceEventNowPlayingChanged;
  s_event_count = 0;
  s_has_now_playing = false;
  s_title[0] = '\0';
  s_artist[0] = '\0';
  s_album[0] = '\0';
  s_playback_state = MusicPlayStateUnknown;
  s_has_player_name = false;
  s_player_name[0] = '\0';
  s_playback_info = (MusicServicePlaybackInfo){};
  s_last_command = MusicServiceCommandPlay;
  s_command_supported = false;

  fake_event_service_init();
  music_service_state_init(&s_music_service_state);
}

void test_music_service__cleanup(void) {}

void test_music_service__metadata_and_playback_wrappers(void) {
  s_has_now_playing = true;
  strncpy(s_title, "Track", sizeof(s_title));
  strncpy(s_artist, "Artist", sizeof(s_artist));
  strncpy(s_album, "Album", sizeof(s_album));
  s_playback_state = MusicPlayStatePlaying;
  s_has_player_name = true;
  strncpy(s_player_name, "Player", sizeof(s_player_name));
  s_playback_info = (MusicServicePlaybackInfo){
      .position_ms = 1234,
      .duration_ms = 5678,
      .playback_rate_percent = 100,
      .playback_state = MusicServicePlaybackStatePlaying,
      .capabilities = MusicServiceCapabilityPlaybackState | MusicServiceCapabilityProgress |
                      MusicServiceCapabilityVolume,
      .volume_percent = 42,
  };

  char title[MUSIC_SERVICE_BUFFER_LENGTH];
  char artist[MUSIC_SERVICE_BUFFER_LENGTH];
  char album[MUSIC_SERVICE_BUFFER_LENGTH];
  char player_name[MUSIC_SERVICE_BUFFER_LENGTH];
  MusicServicePlaybackInfo playback_info;

  cl_assert(music_service_has_now_playing());
  music_service_get_now_playing(title, artist, album);
  cl_assert_equal_s(title, "Track");
  cl_assert_equal_s(artist, "Artist");
  cl_assert_equal_s(album, "Album");
  cl_assert_equal_i(music_service_get_playback_state(), MusicServicePlaybackStatePlaying);
  cl_assert(music_service_get_player_name(player_name));
  cl_assert_equal_s(player_name, "Player");

  music_service_get_playback_info(&playback_info);
  cl_assert_equal_i(playback_info.position_ms, 1234);
  cl_assert_equal_i(playback_info.duration_ms, 5678);
  cl_assert_equal_i(playback_info.playback_rate_percent, 100);
  cl_assert_equal_i(playback_info.playback_state, MusicServicePlaybackStatePlaying);
  cl_assert_equal_i(playback_info.capabilities, s_playback_info.capabilities);
  cl_assert_equal_i(playback_info.volume_percent, 42);
}

void test_music_service__maps_private_playback_states(void) {
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

  for (size_t i = 0; i < ARRAY_LENGTH(cases); i++) {
    s_playback_state = cases[i].private_state;
    cl_assert_equal_i(music_service_get_playback_state(), cases[i].public_state);
  }
}

void test_music_service__command_wrappers(void) {
  s_command_supported = true;
  cl_assert(music_service_is_command_supported(MusicServiceCommandNextTrack));
  cl_assert_equal_i(s_last_command, MusicServiceCommandNextTrack);

  cl_assert(music_service_send_command(MusicServiceCommandVolumeUp));
  cl_assert_equal_i(s_last_command, MusicServiceCommandVolumeUp);

  s_command_supported = false;
  cl_assert(!music_service_send_command(MusicServiceCommandDislike));
  cl_assert_equal_i(s_last_command, MusicServiceCommandDislike);
}

void test_music_service__maps_all_media_events(void) {
  const struct {
    PebbleMediaEventType private_event;
    MusicServiceEventType public_event;
  } cases[] = {
      {PebbleMediaEventTypeNowPlayingChanged, MusicServiceEventNowPlayingChanged},
      {PebbleMediaEventTypePlaybackStateChanged, MusicServiceEventPlaybackStateChanged},
      {PebbleMediaEventTypeVolumeChanged, MusicServiceEventVolumeChanged},
      {PebbleMediaEventTypeTrackPosChanged, MusicServiceEventTrackPositionChanged},
      {PebbleMediaEventTypeServerConnected, MusicServiceEventServerConnected},
      {PebbleMediaEventTypeServerDisconnected, MusicServiceEventServerDisconnected},
  };

  music_service_subscribe(prv_event_handler);
  EventServiceInfo *event_info = fake_event_service_get_info(PEBBLE_MEDIA_EVENT);
  cl_assert(event_info->handler);

  for (size_t i = 0; i < ARRAY_LENGTH(cases); i++) {
    PebbleEvent event = {
        .type = PEBBLE_MEDIA_EVENT,
        .media.type = cases[i].private_event,
    };
    event_info->handler(&event, event_info->context);
    cl_assert_equal_i(s_last_event, cases[i].public_event);
    cl_assert_equal_i(s_event_count, i + 1);
  }

  music_service_unsubscribe();
  cl_assert_equal_p(fake_event_service_get_info(PEBBLE_MEDIA_EVENT)->handler, NULL);
}
