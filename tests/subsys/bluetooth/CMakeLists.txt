# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

set(nimble third_party/nimble/mynewt-nimble)

pbl_clar_test(test_nimble_gatt
  SOURCES
    subsys/bluetooth/gatt.c
    subsys/bluetooth/nimble_type_conversions.c
  INCLUDES
    subsys/bluetooth
    third_party/nimble/port/include
    ${nimble}/nimble/include
    ${nimble}/nimble/host/include
    ${nimble}/nimble/host/services/gatt/include
    ${nimble}/nimble/transport/include
    ${nimble}/porting/nimble/include
  DEFINES CONFIG_BT_LOG_LEVEL=0 CONFIG_NIMBLE_LOG_LEVEL=0
)

pbl_clar_test(test_nimble_conn_lookup
  SOURCES
    subsys/bluetooth/nimble_type_conversions.c
  INCLUDES
    subsys/bluetooth
    third_party/nimble/port/include
    ${nimble}/nimble/include
    ${nimble}/nimble/host/include
    ${nimble}/nimble/transport/include
    ${nimble}/porting/nimble/include
  DEFINES CONFIG_BT_LOG_LEVEL=0 CONFIG_NIMBLE_LOG_LEVEL=0
)
