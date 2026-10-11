# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_peek_widgets
  SOURCES
    fw/services/peek_widgets/peek_widgets.c
    fw/services/timeline/attribute.c
    fw/services/timeline/attribute_group.c
    fw/services/timeline/attributes_actions.c
    fw/services/timeline/item.c
    fw/services/clock/time.c
    tests/fakes/fake_rtc.c
  DEFINES CONFIG_SERVICE_PEEK_WIDGETS=1
  OVERRIDES dummy_board
)
