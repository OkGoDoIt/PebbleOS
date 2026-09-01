/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "pbl/services/music.h"
#include "pbl/services/music_internal.h"

#include "kernel/events.h"

#include <string.h>

// Stubs & Fakes
///////////////////////////////////////////////////////////

#include "fake_events.h"
#include "fake_rtc.h"

#include "stubs_app_manager.h"
#include "stubs_app_install_manager.h"
#include "stubs_bt_lock.h"
#include "stubs_hexdump.h"
#include "stubs_imaging.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_serial.h"
#include "stubs_tick.h"

void ams_music_disconnect(void) {}

// Helpers
///////////////////////////////////////////////////////////

#define prv_set_title(str) music_update_track_title(str, strlen(str))
#define prv_set_artist(str) music_update_track_artist(str, strlen(str))
#define prv_set_album(str) music_update_track_album(str, strlen(str))
#define prv_set_player(str) music_update_player_name(str, strlen(str))

static void prv_set_now_playing(const char *title, const char *artist, const char *album) {
  music_update_now_playing(title, strlen(title), artist, strlen(artist), album, strlen(album));
}

//! The generation is a wrapping uint8_t and the service state is static across the tests in this
//! binary, so every assertion is on the delta rather than an absolute value.
static uint8_t s_generation_mark;

static void prv_mark_generation(void) {
  s_generation_mark = music_get_now_playing_generation();
}

static uint8_t prv_generation_delta(void) {
  return (uint8_t)(music_get_now_playing_generation() - s_generation_mark);
}

static void prv_assert_now_playing(const char *title, const char *artist, const char *album) {
  char actual_title[MUSIC_BUFFER_LENGTH];
  char actual_artist[MUSIC_BUFFER_LENGTH];
  char actual_album[MUSIC_BUFFER_LENGTH];
  music_get_now_playing(actual_title, actual_artist, actual_album);
  cl_assert_equal_s(actual_title, title);
  cl_assert_equal_s(actual_artist, artist);
  cl_assert_equal_s(actual_album, album);
}

void test_music_service__initialize(void) {
  fake_event_init();
  fake_rtc_init(0, 0);
  music_init();
  // Start every test from a known, empty track.
  music_update_now_playing(NULL, 0, NULL, 0, NULL, 0);
  prv_mark_generation();
}

void test_music_service__cleanup(void) {
}

// The bulk path (Pebble Protocol / Android)
///////////////////////////////////////////////////////////

void test_music_service__bulk_update_bumps_generation_once_per_track(void) {
  prv_set_now_playing("Safe & Sound", "Fiji Blue", "Sunkissed");
  cl_assert_equal_i(prv_generation_delta(), 1);
  prv_assert_now_playing("Safe & Sound", "Fiji Blue", "Sunkissed");

  prv_set_now_playing("Cold Feet", "Fiji Blue", "Sunkissed");
  cl_assert_equal_i(prv_generation_delta(), 2);
}

void test_music_service__bulk_update_with_no_change_does_not_bump(void) {
  prv_set_now_playing("Safe & Sound", "Fiji Blue", "Sunkissed");
  prv_mark_generation();

  // Phones re-send the current track on reconnect and on unrelated state changes; that is not a
  // new track and must not invalidate the album art we already fetched.
  prv_set_now_playing("Safe & Sound", "Fiji Blue", "Sunkissed");
  cl_assert_equal_i(prv_generation_delta(), 0);
}

// The per-field path (AMS / iOS)
///////////////////////////////////////////////////////////

void test_music_service__each_track_field_bumps_the_generation(void) {
  prv_set_title("Safe & Sound");
  cl_assert_equal_i(prv_generation_delta(), 1);

  prv_set_artist("Fiji Blue");
  cl_assert_equal_i(prv_generation_delta(), 2);

  prv_set_album("Sunkissed");
  cl_assert_equal_i(prv_generation_delta(), 3);

  prv_assert_now_playing("Safe & Sound", "Fiji Blue", "Sunkissed");
}

void test_music_service__unchanged_track_field_does_not_bump(void) {
  prv_set_title("Safe & Sound");
  prv_set_artist("Fiji Blue");
  prv_mark_generation();

  // AMS re-sends attributes it has already sent (e.g. after a reconnect or a playback state
  // change). Re-fetching album art for each of those would be pure waste.
  prv_set_title("Safe & Sound");
  prv_set_artist("Fiji Blue");
  cl_assert_equal_i(prv_generation_delta(), 0);
}

void test_music_service__player_name_does_not_bump_the_generation(void) {
  prv_set_title("Safe & Sound");
  prv_mark_generation();

  // The player is not part of the track's identity: switching apps mid-track, or the name simply
  // arriving late, is not a new song.
  prv_set_player("Apple Music");
  cl_assert_equal_i(prv_generation_delta(), 0);
}

void test_music_service__truncated_field_change_below_the_cut_does_not_bump(void) {
  char long_title[MUSIC_BUFFER_LENGTH + 32];
  memset(long_title, 'a', sizeof(long_title) - 1);
  long_title[sizeof(long_title) - 1] = '\0';
  prv_set_title(long_title);
  prv_mark_generation();

  // Only the bytes we actually keep can differ, so a change past the truncation point is invisible
  // and must not read as a track change.
  long_title[MUSIC_BUFFER_LENGTH + 4] = 'b';
  prv_set_title(long_title);
  cl_assert_equal_i(prv_generation_delta(), 0);
}

//! The user-visible regression: on iOS the metadata arrives field-by-field, artist first (AMS
//! attribute 0) and title after (attribute 2). Consumers that only re-read when the generation
//! moves - the Quick View music widget, the Music app's album art request - used to see no bump at
//! all here, so they rendered the new artist beside the PREVIOUS track's title and then ignored the
//! title update, leaving the title one track behind for as long as music kept playing.
void test_music_service__ams_style_track_change_is_visible_after_every_field(void) {
  prv_set_artist("Fiji Blue");
  prv_set_album("Sunkissed");
  prv_set_title("Safe & Sound");
  prv_mark_generation();

  // Next track, same artist: the artist is unchanged, so only album and title move the generation.
  prv_set_artist("Fiji Blue");
  cl_assert_equal_i(prv_generation_delta(), 0);

  prv_set_album("Cold Feet");
  const uint8_t after_album = prv_generation_delta();
  cl_assert_equal_i(after_album, 1);

  prv_set_title("Cold Feet");
  cl_assert_equal_i(prv_generation_delta(), 2);

  // Whatever a consumer redrew on the way through, the last bump leaves it reading the new track.
  prv_assert_now_playing("Cold Feet", "Fiji Blue", "Cold Feet");
}

void test_music_service__now_playing_changed_event_fires_for_every_field(void) {
  // The generation gates re-reads, but the event is what wakes the consumers up in the first place,
  // so it must still fire even when nothing changed.
  fake_event_clear_last();
  prv_set_title("Safe & Sound");
  cl_assert_equal_i(fake_event_get_last().type, PEBBLE_MEDIA_EVENT);
  cl_assert_equal_i(fake_event_get_last().media.type, PebbleMediaEventTypeNowPlayingChanged);

  fake_event_clear_last();
  prv_set_title("Safe & Sound");
  cl_assert_equal_i(fake_event_get_last().type, PEBBLE_MEDIA_EVENT);
  cl_assert_equal_i(fake_event_get_last().media.type, PebbleMediaEventTypeNowPlayingChanged);
}
