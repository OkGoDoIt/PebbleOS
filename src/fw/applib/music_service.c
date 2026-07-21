/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "music_service.h"
#include "music_service_private.h"

#include "event_service_client.h"
#include "kernel/events.h"
#include "syscall/syscall.h"
#include "system/passert.h"

#include "process_state/app_state/app_state.h"
#include "process_state/worker_state/worker_state.h"


// ----------------------------------------------------------------------------------------------------
static MusicServiceState* prv_get_state(PebbleTask task) {
  if (task == PebbleTask_Unknown) {
    task = pebble_task_get_current();
  }

  if (task == PebbleTask_App) {
    return app_state_get_music_service_state();
  } else if (task == PebbleTask_Worker) {
    return worker_state_get_music_service_state();
  } else {
    WTF;
  }
}


static void do_handle(PebbleEvent *e, void *context) {
  MusicServiceState *state = prv_get_state(PebbleTask_Unknown);
  PBL_ASSERTN(state->handler != NULL);
  switch (e->media.type) {
    case PebbleMediaEventTypeNowPlayingChanged:
      state->handler(MusicServiceEventNowPlayingChanged);
      break;
    case PebbleMediaEventTypePlaybackStateChanged:
      state->handler(MusicServiceEventPlaybackStateChanged);
      break;
    case PebbleMediaEventTypeVolumeChanged:
      state->handler(MusicServiceEventVolumeChanged);
      break;
    case PebbleMediaEventTypeTrackPosChanged:
      state->handler(MusicServiceEventTrackPositionChanged);
      break;
    case PebbleMediaEventTypeServerConnected:
      state->handler(MusicServiceEventServerConnected);
      break;
    case PebbleMediaEventTypeServerDisconnected:
      state->handler(MusicServiceEventServerDisconnected);
      break;
  }
}

bool music_service_has_now_playing(void) {
  return sys_music_has_now_playing();
}

void music_service_get_now_playing(char *title, char *artist, char *album) {
  sys_music_get_now_playing(title, artist, album);
}

MusicServicePlaybackState music_service_get_playback_state(void) {
  switch (sys_music_get_playback_state()) {
    case MusicPlayStatePlaying:
      return MusicServicePlaybackStatePlaying;
    case MusicPlayStatePaused:
      return MusicServicePlaybackStatePaused;
    case MusicPlayStateForwarding:
      return MusicServicePlaybackStateForwarding;
    case MusicPlayStateRewinding:
      return MusicServicePlaybackStateRewinding;
    case MusicPlayStateUnknown:
    case MusicPlayStateInvalid:
    default:
      return MusicServicePlaybackStateUnknown;
  }
}

bool music_service_get_player_name(char *player_name) {
  return sys_music_get_player_name(player_name);
}

void music_service_get_playback_info(MusicServicePlaybackInfo *playback_info) {
  sys_music_get_playback_info(playback_info);
}

bool music_service_is_command_supported(MusicServiceCommand command) {
  return sys_music_is_command_supported(command);
}

bool music_service_send_command(MusicServiceCommand command) {
  return sys_music_send_command(command);
}

void music_service_subscribe(MusicServiceEventHandler handler) {
  MusicServiceState *state = prv_get_state(PebbleTask_Unknown);
  state->handler = handler;
  event_service_client_subscribe(&state->mss_info);
}

void music_service_unsubscribe(void) {
  MusicServiceState *state = prv_get_state(PebbleTask_Unknown);
  event_service_client_unsubscribe(&state->mss_info);
  state->handler = NULL;
}

void music_service_state_init(MusicServiceState *state) {
  *state = (MusicServiceState) {
    .mss_info = {
      .type = PEBBLE_MEDIA_EVENT,
      .handler = &do_handle,
    },
  };
}
