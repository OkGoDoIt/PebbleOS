/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! @addtogroup Foundation
//! @{
//!   @addtogroup EventService
//!   @{
//!     @addtogroup MusicService
//!
//! \brief Access to now-playing information and playback controls
//!
//! The MusicService API lets watchfaces and apps read the now-playing music
//! metadata and playback state that the connected phone reports to the watch.
//! Apps can also send playback commands supported by the connected phone. It
//! is the same service used by the built-in Music app.
//! @{

//! Required size in bytes, including the null terminator, of each buffer
//! passed to \ref music_service_get_now_playing().
#define MUSIC_SERVICE_BUFFER_LENGTH 64

//! Playback state of the connected music player
typedef enum {
  //! The playback state is not known
  MusicServicePlaybackStateUnknown = 0,
  //! The player is playing
  MusicServicePlaybackStatePlaying,
  //! The player is paused
  MusicServicePlaybackStatePaused,
  //! The player is fast-forwarding
  MusicServicePlaybackStateForwarding,
  //! The player is rewinding
  MusicServicePlaybackStateRewinding,
} MusicServicePlaybackState;

//! Optional information reported by the connected music service.
typedef enum {
  //! Playback state and rate are available.
  MusicServiceCapabilityPlaybackState = 1 << 0,
  //! Track position and duration are available.
  MusicServiceCapabilityProgress = 1 << 1,
  //! Player volume is available.
  MusicServiceCapabilityVolume = 1 << 2,
} MusicServiceCapability;

//! Current playback information.
typedef struct {
  //! Estimated position in the current track, in milliseconds.
  uint32_t position_ms;
  //! Duration of the current track, in milliseconds.
  uint32_t duration_ms;
  //! Playback rate as a percentage, where 100 is normal and 0 is paused.
  int32_t playback_rate_percent;
  //! Current playback state.
  MusicServicePlaybackState playback_state;
  //! Bitwise OR of supported \ref MusicServiceCapability values.
  uint8_t capabilities;
  //! Current player volume in the range 0 to 100.
  uint8_t volume_percent;
} MusicServicePlaybackInfo;

//! Command sent to the connected music player.
typedef enum {
  //! Start playback.
  MusicServiceCommandPlay = 0,
  //! Pause playback.
  MusicServiceCommandPause,
  //! Toggle between playing and paused.
  MusicServiceCommandTogglePlayPause,
  //! Skip to the next track.
  MusicServiceCommandNextTrack,
  //! Return to the previous track.
  MusicServiceCommandPreviousTrack,
  //! Increase the player volume.
  MusicServiceCommandVolumeUp,
  //! Decrease the player volume.
  MusicServiceCommandVolumeDown,
  //! Cycle through the player's repeat modes.
  MusicServiceCommandAdvanceRepeatMode,
  //! Cycle through the player's shuffle modes.
  MusicServiceCommandAdvanceShuffleMode,
  //! Seek forward by the player's standard interval.
  MusicServiceCommandSkipForward,
  //! Seek backward by the player's standard interval.
  MusicServiceCommandSkipBackward,
  //! Mark the current item as liked.
  MusicServiceCommandLike,
  //! Mark the current item as disliked.
  MusicServiceCommandDislike,
  //! Bookmark the current item.
  MusicServiceCommandBookmark,
} MusicServiceCommand;

//! Type of music service event
typedef enum {
  //! The now-playing metadata (title, artist, album or player name) changed.
  //! Also emitted when a music server connects or disconnects, since the
  //! metadata is (re)set at those points.
  MusicServiceEventNowPlayingChanged = 0,
  //! The playback state changed
  MusicServiceEventPlaybackStateChanged,
  //! The player volume changed.
  MusicServiceEventVolumeChanged,
  //! The reported track position or duration changed.
  MusicServiceEventTrackPositionChanged,
  //! A phone music service connected.
  MusicServiceEventServerConnected,
  //! The phone music service disconnected.
  MusicServiceEventServerDisconnected,
} MusicServiceEventType;

//! Callback type for music service events
//! @param event_type The type of event that occurred
typedef void (*MusicServiceEventHandler)(MusicServiceEventType event_type);

//! @return True if now-playing metadata is currently available.
bool music_service_has_now_playing(void);

//! Copy the current now-playing metadata into the provided buffers. Each
//! buffer must be at least \ref MUSIC_SERVICE_BUFFER_LENGTH bytes; fields
//! longer than the buffer are truncated. Fields that are unknown are set to
//! the empty string.
//! @param title Buffer to receive the track title. Must not be NULL.
//! @param artist Buffer to receive the artist name. Must not be NULL.
//! @param album Buffer to receive the album name. Must not be NULL.
void music_service_get_now_playing(char *title, char *artist, char *album);

//! @return The current playback state of the connected music player.
MusicServicePlaybackState music_service_get_playback_state(void);

//! Copy the name of the active music player into the provided buffer.
//! @param player_name Buffer of at least \ref MUSIC_SERVICE_BUFFER_LENGTH bytes.
//! Must not be NULL.
//! @return True if the player name is available, otherwise false. When false,
//! the buffer is set to the empty string.
bool music_service_get_player_name(char *player_name);

//! Copy the current playback information into the provided structure. Check
//! the `capabilities` field before using optional values. Unsupported values
//! are set to zero or \ref MusicServicePlaybackStateUnknown.
//! @param playback_info Structure to receive playback information. Must not be NULL.
void music_service_get_playback_info(MusicServicePlaybackInfo *playback_info);

//! Check whether the connected phone's music transport supports a playback
//! command. A supported command may still be ignored by the active player.
//! @param command The command to check.
//! @return True if the connected music transport supports the command.
bool music_service_is_command_supported(MusicServiceCommand command);

//! Send a playback command to the connected phone. Delivery is best-effort.
//! @param command The command to send.
//! @return True if the command is supported and was handed to the music
//! service, otherwise false.
bool music_service_send_command(MusicServiceCommand command);

//! Subscribe to the music event service. Once subscribed, the handler gets
//! called whenever the music service publishes a change.
//! @param handler A callback to be executed on music service events
void music_service_subscribe(MusicServiceEventHandler handler);

//! Unsubscribe from the music event service. Once unsubscribed, the
//! previously registered handler will no longer be called.
void music_service_unsubscribe(void);

//!     @} // end addtogroup MusicService
//!   @} // end addtogroup EventService
//! @} // end addtogroup Foundation
