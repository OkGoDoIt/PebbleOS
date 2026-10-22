# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

set(sources
  audio_companion.c
  auth.c
  protocol.c
  reboot_trace.c
  reboot_trace_storage.c
  spool.c
)
if(CONFIG_SERVICE_AUDIO_COMPANION_MUSE)
  list(APPEND sources muse_setup_relay.c)
endif()
if(CONFIG_SERVICE_AUDIO_COMPANION_PTT)
  list(APPEND sources assistant_view.c watch_actions.c watch_actions_platform.c watch_actions_alarms.c)
endif()
pbl_library()
pbl_library_sources(${sources})
