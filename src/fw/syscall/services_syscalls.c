/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "syscall/syscall_internal.h"

#include "applib/music_service.h"
#include "applib/peek_widget_private.h"
#include "kernel/pebble_tasks.h"
#include "process_management/app_manager.h"
#include "process_management/worker_manager.h"
#include "pbl/services/alarms/alarm.h"
#include "pbl/services/music.h"
#include "pbl/services/peek_widgets.h"
#include "shell/prefs.h"

#include <string.h>

_Static_assert(MUSIC_SERVICE_BUFFER_LENGTH == MUSIC_BUFFER_LENGTH,
               "Music service buffer lengths must match");

DEFINE_SYSCALL(bool, sys_alarm_get_next_enabled, time_t *timestamp_out) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(timestamp_out, sizeof(*timestamp_out));
  }
  return alarm_get_next_enabled_alarm(timestamp_out);
}

DEFINE_SYSCALL(bool, sys_hrm_manager_is_hrm_present) {
#ifdef CONFIG_SERVICE_HRM
  return true;
#else
  return false;
#endif
}

DEFINE_SYSCALL(bool, sys_music_has_now_playing) {
#ifdef CONFIG_SERVICE_MUSIC
  return music_has_now_playing();
#else
  return false;
#endif
}

DEFINE_SYSCALL(void, sys_music_get_now_playing, char *title, char *artist, char *album) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(title, MUSIC_SERVICE_BUFFER_LENGTH);
    syscall_assert_userspace_buffer(artist, MUSIC_SERVICE_BUFFER_LENGTH);
    syscall_assert_userspace_buffer(album, MUSIC_SERVICE_BUFFER_LENGTH);
  }
#ifdef CONFIG_SERVICE_MUSIC
  music_get_now_playing(title, artist, album);
#else
  title[0] = '\0';
  artist[0] = '\0';
  album[0] = '\0';
#endif
}

DEFINE_SYSCALL(MusicPlayState, sys_music_get_playback_state) {
#ifdef CONFIG_SERVICE_MUSIC
  return music_get_playback_state();
#else
  return MusicPlayStateUnknown;
#endif
}

#ifdef CONFIG_SERVICE_MUSIC
static MusicServicePlaybackState prv_public_playback_state(MusicPlayState state) {
  switch (state) {
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
#endif

DEFINE_SYSCALL(bool, sys_music_get_player_name, char *player_name) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(player_name, MUSIC_SERVICE_BUFFER_LENGTH);
  }
#ifdef CONFIG_SERVICE_MUSIC
  return music_get_player_name(player_name);
#else
  player_name[0] = '\0';
  return false;
#endif
}

DEFINE_SYSCALL(void, sys_music_get_playback_info, MusicServicePlaybackInfo *playback_info) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(playback_info, sizeof(*playback_info));
  }

  *playback_info = (MusicServicePlaybackInfo){
      .playback_state = MusicServicePlaybackStateUnknown,
  };

#ifdef CONFIG_SERVICE_MUSIC
  if (music_is_playback_state_reporting_supported()) {
    playback_info->capabilities |= MusicServiceCapabilityPlaybackState;
    playback_info->playback_state = prv_public_playback_state(music_get_playback_state());
    playback_info->playback_rate_percent = music_get_playback_rate_percent();
  }
  if (music_is_progress_reporting_supported()) {
    playback_info->capabilities |= MusicServiceCapabilityProgress;
    music_get_pos(&playback_info->position_ms, &playback_info->duration_ms);
  }
  if (music_is_volume_reporting_supported()) {
    playback_info->capabilities |= MusicServiceCapabilityVolume;
    playback_info->volume_percent = music_get_volume_percent();
  }
#endif
}

#ifdef CONFIG_SERVICE_MUSIC
static MusicCommand prv_private_music_command(MusicServiceCommand command) {
  switch (command) {
    case MusicServiceCommandPlay:
      return MusicCommandPlay;
    case MusicServiceCommandPause:
      return MusicCommandPause;
    case MusicServiceCommandTogglePlayPause:
      return MusicCommandTogglePlayPause;
    case MusicServiceCommandNextTrack:
      return MusicCommandNextTrack;
    case MusicServiceCommandPreviousTrack:
      return MusicCommandPreviousTrack;
    case MusicServiceCommandVolumeUp:
      return MusicCommandVolumeUp;
    case MusicServiceCommandVolumeDown:
      return MusicCommandVolumeDown;
    case MusicServiceCommandAdvanceRepeatMode:
      return MusicCommandAdvanceRepeatMode;
    case MusicServiceCommandAdvanceShuffleMode:
      return MusicCommandAdvanceShuffleMode;
    case MusicServiceCommandSkipForward:
      return MusicCommandSkipForward;
    case MusicServiceCommandSkipBackward:
      return MusicCommandSkipBackward;
    case MusicServiceCommandLike:
      return MusicCommandLike;
    case MusicServiceCommandDislike:
      return MusicCommandDislike;
    case MusicServiceCommandBookmark:
      return MusicCommandBookmark;
    default:
      return NumMusicCommand;
  }
}
#endif

DEFINE_SYSCALL(bool, sys_music_is_command_supported, MusicServiceCommand command) {
#ifdef CONFIG_SERVICE_MUSIC
  const MusicCommand private_command = prv_private_music_command(command);
  return private_command != NumMusicCommand && music_is_command_supported(private_command);
#else
  return false;
#endif
}

DEFINE_SYSCALL(bool, sys_music_send_command, MusicServiceCommand command) {
#ifdef CONFIG_SERVICE_MUSIC
  const MusicCommand private_command = prv_private_music_command(command);
  if (private_command == NumMusicCommand || !music_is_command_supported(private_command)) {
    return false;
  }
  music_command_send(private_command);
  return true;
#else
  return false;
#endif
}

#ifdef CONFIG_SERVICE_PEEK_WIDGETS
_Static_assert(PEEK_WIDGET_TITLE_MAX_LEN == PEEK_WIDGET_APP_TITLE_MAX_LEN,
               "SDK and service title limits must agree");
_Static_assert(PEEK_WIDGET_SUBTITLE_MAX_LEN == PEEK_WIDGET_APP_SUBTITLE_MAX_LEN,
               "SDK and service subtitle limits must agree");

//! The calling process's UUID, resolved kernel-side so an app can only touch its own widget.
static bool prv_get_calling_process_uuid(Uuid *uuid_out) {
  const PebbleProcessMd *md = (pebble_task_get_current() == PebbleTask_Worker)
      ? worker_manager_get_current_worker_md()
      : app_manager_get_current_app_md();
  if (!md) {
    return false;
  }
  *uuid_out = md->uuid;
  return true;
}
#endif

DEFINE_SYSCALL(PeekWidgetResult, sys_peek_widget_publish, const PeekWidgetPublishArgs *args) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(args, sizeof(*args));
  }
#ifdef CONFIG_SERVICE_PEEK_WIDGETS
  PeekWidgetPublishArgs args_copy = *args;
  args_copy.title[PEEK_WIDGET_TITLE_MAX_LEN] = '\0';
  args_copy.subtitle[PEEK_WIDGET_SUBTITLE_MAX_LEN] = '\0';
  if (args_copy.title[0] == '\0') {
    return PEEK_WIDGET_RESULT_INVALID_ARGS;
  }
  if (!quick_view_prefs_get_apps_enabled()) {
    return PEEK_WIDGET_RESULT_DISABLED;
  }
  PeekWidgetAppPublish publish = {
    .icon = args_copy.icon,
    .title = args_copy.title,
    .subtitle = args_copy.subtitle,
    .launch_code = args_copy.launch_code,
    .timeout_s = args_copy.timeout_s,
  };
  if (!prv_get_calling_process_uuid(&publish.owner)) {
    return PEEK_WIDGET_RESULT_INVALID_ARGS;
  }
  return peek_widgets_publish_app_widget(&publish)
      ? PEEK_WIDGET_RESULT_SUCCESS : PEEK_WIDGET_RESULT_INVALID_ARGS;
#else
  return PEEK_WIDGET_RESULT_DISABLED;
#endif
}

DEFINE_SYSCALL(void, sys_peek_widget_withdraw, void) {
#ifdef CONFIG_SERVICE_PEEK_WIDGETS
  Uuid uuid;
  if (prv_get_calling_process_uuid(&uuid)) {
    peek_widgets_withdraw_app_widget(&uuid);
  }
#endif
}
