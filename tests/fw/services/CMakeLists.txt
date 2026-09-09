# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

# The timezone database the tests check against, as a C array. The
# firmware bakes the same bytes into a resource.
set(timezone_fixture ${CMAKE_CURRENT_BINARY_DIR}/timezone_fixture.auto.h)
set(timezones_olson ${PBL_BASE}/resources/normal/base/tzdata/timezones_olson.txt)
add_custom_command(
  OUTPUT ${timezone_fixture}
  COMMAND ${PYTHON_EXECUTABLE} ${PBL_BASE}/tools/cmake/resources.py tzdata-header
          --input ${timezones_olson} --output ${timezone_fixture}
  DEPENDS ${timezones_olson} ${PBL_BASE}/tools/cmake/resources.py
  COMMENT "Generating timezone test fixture"
  VERBATIM
)
add_custom_target(pbl_timezone_fixture DEPENDS ${timezone_fixture})

pbl_clar_test(test_pfs
  SOURCES
    src/fw/services/filesystem/flash_translation.c
    src/fw/services/filesystem/pfs.c
    src/fw/system/hexdump.c
    src/fw/flash_region/flash_region.c
    src/fw/flash_region/filesystem_regions.c
    tests/fakes/fake_spi_flash.c
    src/fw/util/crc8.c
    src/fw/util/legacy_checksum.c
    tests/fakes/fake_rtc.c
  PLATFORMS obelix
  DEFINES DUMA_DISABLED
  OVERRIDES dummy_board
)

pbl_clar_test(test_put_bytes
  SOURCES
    src/fw/services/put_bytes/put_bytes.c
    src/fw/services/put_bytes/put_bytes_storage.c
    tests/fakes/fake_session.c
    src/fw/util/legacy_checksum.c
    tests/fakes/fake_events.c
    tests/fakes/fake_put_bytes_storage_mem.c
    tests/fakes/fake_resource_storage.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_spi_flash.c
  PLATFORMS obelix gabbro
  OVERRIDES dummy_board
)

pbl_clar_test(test_cron
  SOURCES
    tests/fakes/fake_rtc.c
    src/fw/services/cron/service.c
    src/fw/util/time/time.c
    src/fw/util/time/mktime.c
)

pbl_clar_test(test_contacts
  SOURCES
    tests/fakes/fake_rtc.c
    tests/fakes/fake_spi_flash.c
    src/fw/flash_region/flash_region.c
    src/fw/flash_region/filesystem_regions.c
    src/fw/services/contacts/attributes_address.c
    src/fw/services/contacts/contacts.c
    src/fw/services/blob_db/contacts_db.c
    src/fw/services/filesystem/flash_translation.c
    src/fw/services/filesystem/pfs.c
    src/fw/services/settings/settings_file.c
    src/fw/services/settings/settings_raw_iter.c
    src/fw/services/timeline/item.c
    src/fw/services/timeline/attributes_actions.c
    src/fw/services/timeline/attribute_group.c
    src/fw/services/timeline/attribute.c
    src/fw/util/stringlist.c
    src/fw/util/crc8.c
    src/fw/util/legacy_checksum.c
    src/fw/util/rand/rand.c
    src/fw/util/time/time.c
    third_party/tinymt/TinyMT/tinymt/tinymt32.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_evented_timer
  SOURCES
    src/fw/services/evented_timer/service.c
)

pbl_clar_test(test_music_endpoint
  SOURCES
    tests/fakes/fake_events.c
    tests/fakes/fake_session.c
    tests/fakes/fake_rtc.c
    src/fw/services/music/endpoint.c
    src/fw/services/music/service.c
)

pbl_clar_test(test_imaging
  SOURCES
    tests/fakes/fake_session.c
    src/fw/services/imaging/imaging.c
)

pbl_clar_test(test_compass_cal
  SOURCES
    src/fw/services/ecompass/correction.c
)

pbl_clar_test(test_light
  SOURCES
    src/fw/services/light/service.c
  OVERRIDES getafix_backlight
)

pbl_clar_test(test_als_screen_compensation
  SOURCES
    src/fw/services/light/als_screen_compensation_math.c
    src/fw/services/light/als_screen_compensation_region.c
  DEFINES CONFIG_ALS_SCREEN_COMPENSATION CONFIG_AMBIENT_LIGHT_BITS=16
  OVERRIDES dummy_board
)

pbl_clar_test(test_phone_pp
  SOURCES
    tests/fakes/fake_events.c
    tests/fakes/fake_session.c
    src/fw/services/phone_call/util.c
    src/fw/services/phone_pp/service.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_phone_call
  SOURCES
    src/fw/services/phone_call/service.c
)

pbl_clar_test(test_debounced_connection_service
  SOURCES
    src/fw/services/regular_timer/service.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_session.c
    src/fw/services/debounced_connection_service/service.c
)

pbl_clar_test(test_timeline_item
  SOURCES
    tests/fakes/fake_rtc.c
    src/fw/util/rand/rand.c
    third_party/tinymt/TinyMT/tinymt/tinymt32.c
    src/fw/services/timeline/item.c
    src/fw/services/timeline/attributes_actions.c
    src/fw/services/timeline/attribute_group.c
    src/fw/services/timeline/attribute.c
    src/fw/util/stringlist.c
    src/fw/util/time/time.c
)

pbl_clar_test(test_clock
  SOURCES
    src/fw/services/i18n/i18n.c
    src/fw/flash_region/flash_region.c
    src/fw/flash_region/filesystem_regions.c
    src/fw/resource/resource.c
    src/fw/resource/resource_storage.c
    src/fw/resource/resource_storage_builtin.c
    src/fw/resource/resource_storage_file.c
    src/fw/resource/resource_storage_flash.c
    src/fw/services/filesystem/flash_translation.c
    src/fw/services/filesystem/pfs.c
    src/fw/services/filesystem/app_file.c
    src/fw/util/time/time.c
    src/fw/util/time/mktime.c
    tests/fakes/fake_bootbits.c
    src/fw/util/crc8.c
    src/fw/util/legacy_checksum.c
    src/fw/drivers/flash/flash_crc.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_events.c
    tests/fakes/fake_spi_flash.c
    tests/fixtures/resources/builtin_resources.auto.c
    tests/fixtures/resources/pfs_resource_table.c
    src/fw/services/timezone_database/service.c
    src/fw/services/clock/service.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_timezone_database
  SOURCES
    tests/fakes/fake_spi_flash.c
    src/fw/services/timezone_database/service.c
  OVERRIDES test_timezone  DEPENDS pbl_timezone_fixture
)

pbl_clar_test(test_wakeup
  SOURCES
    src/fw/flash_region/flash_region.c
    src/fw/flash_region/filesystem_regions.c
    src/fw/util/rand/rand.c
    third_party/tinymt/TinyMT/tinymt/tinymt32.c
    src/fw/process_management/pebble_process_info.c
    src/fw/resource/resource.c
    src/fw/resource/resource_storage.c
    src/fw/resource/resource_storage_builtin.c
    src/fw/resource/resource_storage_file.c
    src/fw/resource/resource_storage_flash.c
    src/fw/services/filesystem/flash_translation.c
    src/fw/services/filesystem/pfs.c
    src/fw/services/filesystem/app_file.c
    src/fw/services/settings/settings_file.c
    src/fw/services/settings/settings_raw_iter.c
    src/fw/services/wakeup/service.c
    tests/fakes/fake_bootbits.c
    src/fw/util/crc8.c
    src/fw/util/legacy_checksum.c
    src/fw/drivers/flash/flash_crc.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_spi_flash.c
    tests/fixtures/resources/builtin_resources.auto.c
    tests/fixtures/resources/pfs_resource_table.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_migrate_wakeup
  SOURCES
    src/fw/drivers/flash/flash_crc.c
    src/fw/flash_region/filesystem_regions.c
    src/fw/flash_region/flash_region.c
    src/fw/process_management/pebble_process_info.c
    src/fw/resource/resource.c
    src/fw/resource/resource_storage.c
    src/fw/resource/resource_storage_builtin.c
    src/fw/resource/resource_storage_file.c
    src/fw/resource/resource_storage_flash.c
    src/fw/services/filesystem/app_file.c
    src/fw/services/filesystem/flash_translation.c
    src/fw/services/filesystem/pfs.c
    src/fw/services/settings/settings_file.c
    src/fw/services/settings/settings_raw_iter.c
    src/fw/services/wakeup/service.c
    src/fw/util/crc8.c
    src/fw/util/legacy_checksum.c
    src/fw/util/rand/rand.c
    third_party/tinymt/TinyMT/tinymt/tinymt32.c
    tests/fakes/fake_bootbits.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_spi_flash.c
    tests/fixtures/resources/builtin_resources.auto.c
    tests/fixtures/resources/pfs_resource_table.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_registry_endpoint
  SOURCES
    src/fw/services/registry_endpoint/service.c
)

pbl_clar_test(test_audio_endpoint
  SOURCES
    src/fw/services/audio_endpoint/service.c
    tests/fakes/fake_session.c
)

pbl_clar_test(test_speaker_service
  SOURCES
    src/fw/services/speaker/speaker_service.c
    src/fw/services/speaker/note_sequence.c
    src/fw/services/speaker/pcm_stream.c
    src/fw/services/speaker/track_player.c
    tests/fakes/fake_events.c
  DEFINES CONFIG_SPEAKER=1
  OVERRIDES dummy_board
)

pbl_clar_test(test_transcription
  SOURCES
    src/fw/services/voice/transcription.c
)

pbl_clar_test(test_voice_endpoint
  SOURCES
    src/fw/util/rand/rand.c
    third_party/tinymt/TinyMT/tinymt/tinymt32.c
    src/fw/util/generic_attribute.c
    src/fw/services/voice/transcription.c
    src/fw/services/voice_endpoint/service.c
    tests/fakes/fake_session.c
  DEFINES CONFIG_MIC
  OVERRIDES dummy_board
)

pbl_clar_test(test_app_cache
  SOURCES
    src/fw/flash_region/flash_region.c
    src/fw/flash_region/filesystem_regions.c
    src/fw/services/filesystem/app_file.c
    src/fw/services/filesystem/flash_translation.c
    src/fw/services/filesystem/pfs.c
    src/fw/services/app_cache/service.c
    src/fw/services/settings/settings_file.c
    src/fw/services/settings/settings_raw_iter.c
    src/fw/util/crc8.c
    src/fw/util/legacy_checksum.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_spi_flash.c
    tests/fakes/fake_events.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_app_install_manager
  SOURCES
    src/fw/flash_region/flash_region.c
    src/fw/flash_region/filesystem_regions.c
    src/fw/resource/resource.c
    src/fw/resource/resource_storage.c
    src/fw/resource/resource_storage_builtin.c
    src/fw/resource/resource_storage_file.c
    src/fw/resource/resource_storage_flash.c
    src/fw/services/filesystem/flash_translation.c
    src/fw/services/filesystem/pfs.c
    src/fw/services/filesystem/app_file.c
    src/fw/services/app_cache/service.c
    src/fw/services/process_management/app_storage.c
    src/fw/process_management/app_install_manager.c
    src/fw/process_management/pebble_process_md.c
    src/fw/services/blob_db/app_db.c
    src/fw/services/settings/settings_file.c
    src/fw/services/settings/settings_raw_iter.c
    src/fw/util/rand/rand.c
    third_party/tinymt/TinyMT/tinymt/tinymt32.c
    src/fw/applib/graphics/gcolor_definitions.c
    src/fw/applib/graphics/gtypes.c
    src/fw/process_management/pebble_process_info.c
    src/fw/util/crc8.c
    src/fw/util/legacy_checksum.c
    src/fw/drivers/flash/flash_crc.c
    tests/fakes/fake_rtc.c
    tests/fixtures/resources/builtin_resources.auto.c
    tests/fakes/fake_system_app_registry_apps.c
    tests/fakes/fake_spi_flash.c
  OVERRIDES dummy_board fake_app_registry
)

pbl_clar_test(test_app_menu_data_source
  SOURCES
    src/fw/applib/graphics/gcolor_definitions.c
    src/fw/applib/graphics/gtypes.c
    src/fw/drivers/flash/flash_crc.c
    src/fw/flash_region/filesystem_regions.c
    src/fw/flash_region/flash_region.c
    src/fw/process_management/app_install_manager.c
    src/fw/process_management/app_menu_data_source.c
    src/fw/process_management/pebble_process_info.c
    src/fw/process_management/pebble_process_md.c
    src/fw/resource/resource.c
    src/fw/resource/resource_storage.c
    src/fw/resource/resource_storage_builtin.c
    src/fw/resource/resource_storage_file.c
    src/fw/resource/resource_storage_flash.c
    src/fw/services/app_cache/service.c
    src/fw/services/blob_db/app_db.c
    src/fw/services/filesystem/app_file.c
    src/fw/services/filesystem/flash_translation.c
    src/fw/services/filesystem/pfs.c
    src/fw/services/process_management/app_order_storage.c
    src/fw/services/process_management/app_storage.c
    src/fw/services/settings/settings_file.c
    src/fw/services/settings/settings_raw_iter.c
    src/fw/util/crc8.c
    src/fw/util/legacy_checksum.c
    src/fw/util/rand/rand.c
    third_party/tinymt/TinyMT/tinymt/tinymt32.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_spi_flash.c
    tests/fakes/fake_system_app_registry_apps.c
    tests/fixtures/resources/builtin_resources.auto.c
  OVERRIDES dummy_board fake_app_registry
)

pbl_clar_test(test_app_fetch_endpoint
  SOURCES
    src/fw/util/rand/rand.c
    third_party/tinymt/TinyMT/tinymt/tinymt32.c
    src/fw/services/app_fetch_endpoint/service.c
    tests/fakes/fake_events.c
    tests/fakes/fake_session.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_accel_manager
  SOURCES
    src/fw/util/shared_circular_buffer.c
    src/fw/services/accel_manager/service.c
    tests/fakes/fake_events.c
  DEFINES CONFIG_SERVICE_ACCEL_MANAGER_BATCH_SAMPLES=25
  OVERRIDES dummy_board
)

pbl_clar_test(test_shared_prf_storage_v3
  SOURCES
    src/fw/services/shared_prf_storage/shared_prf_storage.c
    tests/fakes/fake_flash_region.c
    tests/fakes/fake_spi_flash.c
  PLATFORMS obelix
  OVERRIDES shared_prf_storage_v3
)

pbl_clar_test(test_app_file
  SOURCES
    src/fw/services/filesystem/app_file.c
)

pbl_clar_test(test_do_not_disturb
  SOURCES
    src/fw/flash_region/flash_region.c
    src/fw/flash_region/filesystem_regions.c
    src/fw/services/filesystem/flash_translation.c
    src/fw/services/filesystem/pfs.c
    src/fw/services/settings/settings_file.c
    src/fw/services/settings/settings_raw_iter.c
    src/fw/services/notifications/alerts_preferences.c
    src/fw/services/notifications/do_not_disturb.c
    src/fw/util/time/time.c
    src/fw/util/crc8.c
    src/fw/util/legacy_checksum.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_spi_flash.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_weather_service
  SOURCES
    src/fw/util/pstring.c
    src/fw/util/time/time.c
    src/fw/util/crc8.c
    src/fw/util/legacy_checksum.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_spi_flash.c
    src/fw/flash_region/flash_region.c
    src/fw/flash_region/filesystem_regions.c
    src/fw/services/settings/settings_file.c
    src/fw/services/settings/settings_raw_iter.c
    src/fw/services/filesystem/flash_translation.c
    src/fw/services/filesystem/pfs.c
    src/fw/services/blob_db/weather_db.c
    src/fw/services/weather/weather_service.c
    tests/fw/services/blob_db/weather_data_shared.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_vibe
  SOURCES
    src/fw/services/vibe_pattern/service.c
    src/fw/applib/ui/vibes.c
    tests/fakes/fake_events.c
    tests/fakes/fake_rtc.c
)

pbl_clar_test(test_vibe_intensity
  SOURCES
    src/fw/services/vibes/vibe_intensity.c
)

pbl_clar_test(test_vibe_score
  SOURCES
    src/fw/services/vibes/vibe_score.c
    src/fw/util/generic_attribute.c
)

pbl_clar_test(test_vibe_score_info
  SOURCES
    src/fw/services/vibes/vibe_score_info.c
)

pbl_clar_test(test_touch
  SOURCES
    src/fw/services/touch/touch.c
    tests/fakes/fake_events.c
)

pbl_clar_test(test_touch_session
  SOURCES
    src/fw/services/touch/touch_session.c
    tests/fakes/fake_rtc.c
)

pbl_clar_test(test_hrm_manager
  SOURCES
    src/fw/services/hrm/hrm_manager.c
    kernel/tick.c
    tests/fakes/fake_events.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_accel_service.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_app_glance_service
  SOURCES
    src/fw/services/app_glances/app_glance_service.c
    src/fw/services/blob_db/app_glance_db.c
    src/fw/services/timeline/attribute.c
    src/fw/util/crc8.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_settings_file.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_music_service
  SOURCES
    tests/fakes/fake_events.c
    tests/fakes/fake_rtc.c
    src/fw/services/music/service.c
)

pbl_clar_test(test_audio_companion_protocol
  SOURCES
    src/fw/services/audio_companion/protocol.c
)

# The heartbeat the official Core Devices app's Battery screen is built from. See
# UPSTREAM_MERGE_CHECKLIST.md Trap 1: the schema guard checks the record's shape, this checks the
# bytes the firmware actually hands to DLS.
pbl_clar_test(test_analytics_native_heartbeat
  SOURCES
    src/fw/services/analytics/native.c
    src/fw/util/time/time.c
    tests/fakes/fake_rtc.c
)

pbl_clar_test(test_audio_companion_spool
  SOURCES
    src/fw/services/audio_companion/spool.c
)

pbl_clar_test(test_audio_companion_auth
  SOURCES
    src/fw/services/audio_companion/auth.c
    src/fw/services/filesystem/flash_translation.c
    src/fw/services/filesystem/pfs.c
    src/fw/services/settings/settings_file.c
    src/fw/services/settings/settings_raw_iter.c
    src/fw/flash_region/flash_region.c
    src/fw/flash_region/filesystem_regions.c
    src/fw/util/crc8.c
    src/fw/util/legacy_checksum.c
    src/fw/util/rand/rand.c
    src/fw/util/time/time.c
    third_party/tinymt/TinyMT/tinymt/tinymt32.c
    third_party/mbedtls/mbedtls/library/sha256.c
    third_party/mbedtls/mbedtls/library/platform_util.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_spi_flash.c
  INCLUDES
    third_party/mbedtls/mbedtls/include
    third_party/mbedtls/mbedtls/library
    third_party/mbedtls/include
  DEFINES
    MBEDTLS_CONFIG_FILE="pebble_mbedtls_config.h"
    CONFIG_SERVICE_AUDIO_COMPANION=1
  OVERRIDES dummy_board
)

pbl_clar_test(test_audio_companion
  SOURCES
    src/fw/services/audio_companion/audio_companion.c
    src/fw/services/audio_companion/protocol.c
    src/fw/services/audio_companion/reboot_trace.c
    src/fw/services/audio_companion/spool.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_audio_companion_reboot_trace
  SOURCES
    src/fw/services/audio_companion/reboot_trace.c
  OVERRIDES dummy_board
)

# Real-Speex golden bitstream test: compiles the vendored fixed-point encoder and
# decoder for the host with the same defines third_party/speex/CMakeLists.txt uses,
# so the bitstream matches the watch byte-for-byte.
set(audio_companion_speex_sources
  bits.c cb_search.c exc_10_16_table.c exc_10_32_table.c
  exc_20_32_table.c exc_5_256_table.c exc_5_64_table.c exc_8_128_table.c
  filters.c gain_table.c gain_table_lbr.c hexc_10_32_table.c
  hexc_table.c high_lsp_tables.c kiss_fft.c kiss_fftr.c lpc.c lsp.c
  lsp_tables_nb.c ltp.c modes.c modes_wb.c nb_celp.c quant_lsp.c
  sb_celp.c smallft.c speex.c speex_callbacks.c speex_header.c
  stereo.c vbr.c vorbis_psy.c vq.c window.c
)
list(TRANSFORM audio_companion_speex_sources
     PREPEND third_party/speex/speex/libspeex/)

pbl_clar_test(test_audio_companion_speex
  SOURCES ${audio_companion_speex_sources}
  INCLUDES
    third_party/speex
    third_party/speex/speex/include
    third_party/speex/speex/libspeex
  DEFINES
    HAVE_CONFIG_H
    OS_SUPPORT_CUSTOM
    FIXED_POINT
    DISABLE_FLOAT_API
    DISABLE_VBR
    DISABLE_WARNINGS
    DISABLE_NOTIFICATIONS
    "NB_ENC_STACK=(2000*sizeof(spx_sig_t))"
    "NB_DEC_STACK=(1500*sizeof(spx_sig_t))"
  OVERRIDES dummy_board
)
