/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/kernel/barrier.h>
#include <pbl/kernel/mmio.h>

#include <clar.h>

static int s_lock_depth;
static int s_lock_calls;

void pbl_irq_lock(void) {
  s_lock_depth++;
  s_lock_calls++;
}

void pbl_irq_unlock(void) {
  s_lock_depth--;
}

static union {
  uint32_t l;
  uint16_t w[2];
  uint8_t b[4];
} s_reg;

void test_mmio__initialize(void) {
  s_reg.l = 0U;
  s_lock_depth = 0;
  s_lock_calls = 0;
}

void test_mmio__widths(void) {
  pbl_writel(0x11223344U, (uintptr_t)&s_reg.l);
  cl_assert_equal_i(pbl_readl((uintptr_t)&s_reg.l), 0x11223344U);
  cl_assert_equal_i(pbl_readw((uintptr_t)&s_reg.w[0]), 0x3344U);
  cl_assert_equal_i(pbl_readb((uintptr_t)&s_reg.b[0]), 0x44U);

  pbl_writew(0xaabbU, (uintptr_t)&s_reg.w[1]);
  cl_assert_equal_i(pbl_readl((uintptr_t)&s_reg.l), 0xaabb3344U);

  pbl_writeb(0xccU, (uintptr_t)&s_reg.b[1]);
  cl_assert_equal_i(pbl_readl((uintptr_t)&s_reg.l), 0xaabbcc44U);
}

void test_mmio__dma_handoff(void) {
  static uint32_t s_buf[2];

  s_buf[0] = 0x5aU;
  pbl_wmb();
  pbl_writel(1U, (uintptr_t)&s_reg.l);

  cl_assert_equal_i(pbl_readl((uintptr_t)&s_reg.l), 1U);
  pbl_rmb();
  cl_assert_equal_i(s_buf[0], 0x5aU);
}

void test_mmio__modify(void) {
  pbl_writel(0xff00ff00U, (uintptr_t)&s_reg.l);

  pbl_modifyl(0x0000ff00U, 0x000000ffU, (uintptr_t)&s_reg.l);
  cl_assert_equal_i(pbl_readl((uintptr_t)&s_reg.l), 0xff0000ffU);

  pbl_modifyw(0x00ffU, 0x0f00U, (uintptr_t)&s_reg.w[0]);
  cl_assert_equal_i(pbl_readl((uintptr_t)&s_reg.l), 0xff000f00U);

  pbl_modifyb(0x0fU, 0xf0U, (uintptr_t)&s_reg.b[3]);
  cl_assert_equal_i(pbl_readl((uintptr_t)&s_reg.l), 0xf0000f00U);

  cl_assert_equal_i(s_lock_calls, 3);
  cl_assert_equal_i(s_lock_depth, 0);
}

void test_mmio__modify_set_wins(void) {
  pbl_modifyl(0x1U, 0x1U, (uintptr_t)&s_reg.l);
  cl_assert_equal_i(pbl_readl((uintptr_t)&s_reg.l), 0x1U);
}
