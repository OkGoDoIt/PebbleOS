# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_peek_widgets
  SOURCES
    pebble/services/peek_widgets/peek_widgets.c
    pebble/services/timeline/attribute.c
    pebble/services/timeline/attribute_group.c
    pebble/services/timeline/attributes_actions.c
    pebble/services/timeline/item.c
    pebble/services/clock/time.c
    tests/fakes/fake_rtc.c
  DEFINES CONFIG_SERVICE_PEEK_WIDGETS=1 CONFIG_SERVICE_AUDIO_COMPANION=1
  OVERRIDES dummy_board
)
