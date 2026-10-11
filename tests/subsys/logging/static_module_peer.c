/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/kernel/compiler.h>

// Exercise the production module macros with this suite's explicit Kconfig symbols.
#undef UNITTEST
#include <pbl/logging/logging.h>
#define UNITTEST 1

PBL_LOG_MODULE_DEFINE(test_static, LOG_LEVEL_INFO);

void test_log_static_peer(int *evaluations) {
  PBL_LOG_DBG("static debug %d", ++*evaluations);
  PBL_LOG_INFO("static info");
}
