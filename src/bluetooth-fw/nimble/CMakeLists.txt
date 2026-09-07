# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

set(excludes)
if(NOT CONFIG_GH3X2X_TUNING_SERVICE_ENABLED)
  list(APPEND excludes
    gh3x2x_tuning_service.c
  )
endif()
if(NOT CONFIG_AUDIO_COMPANION_SERVICE_ENABLED)
  list(APPEND excludes
    audio_companion_service.c
  )
endif()
file(GLOB driver_source CONFIGURE_DEPENDS *.c)
pbl_filter_sources(driver_source EXCLUDE ${excludes})
pbl_library_named(bt_driver)
pbl_library_sources(${driver_source})
