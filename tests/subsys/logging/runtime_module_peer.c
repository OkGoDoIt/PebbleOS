/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/kernel/compiler.h>

// Exercise the production module macros with this suite's explicit Kconfig symbols.
#undef UNITTEST
#include <pbl/logging/logging.h>
#define UNITTEST 1

PBL_LOG_MODULE_DECLARE(test_runtime, CONFIG_TEST_RUNTIME_LOG_LEVEL);

void test_log_runtime_peer(void) {
  PBL_LOG_DBG("peer message");
}
