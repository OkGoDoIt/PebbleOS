# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_peek_widgets
  SOURCES
    src/fw/services/peek_widgets/peek_widgets.c
    src/fw/services/timeline/attribute.c
    src/fw/services/timeline/attribute_group.c
    src/fw/services/timeline/attributes_actions.c
    src/fw/services/timeline/item.c
    src/fw/util/time/time.c
    tests/fakes/fake_rtc.c
  DEFINES CONFIG_SERVICE_PEEK_WIDGETS=1
  OVERRIDES dummy_board
)
