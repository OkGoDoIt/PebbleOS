/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/flash.h>
#include <pbl/drivers/ram.h>
#include <pbl/kernel/assert.h>
#include <pbl/kernel/idle.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/mmio.h>
#include <pbl/kernel/section.h>
#include <pbl/kernel/time.h>
#include <pbl/soc/sf32lb/idle.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

#ifdef CONFIG_SHELL
#include <pbl/shell/shell.h>
#endif

#include <devicetree/idle_states.h>
#include <devicetree/regs.h>
#include <hardware/sf32lb52_hpsys_aon.h>
#include <hardware/sf32lb52_hpsys_rcc.h>

#define HPSYS_AON(reg) (PBL_DT_REG_HPSYS_AON + SF32LB52_HPSYS_AON_##reg##_OFFSET)
#define HPSYS_RCC(reg) (PBL_DT_REG_HPSYS_RCC + SF32LB52_HPSYS_RCC_##reg##_OFFSET)

// Settling time after waking up from deep sleep
#define DEEPSLEEP_EXIT_NOPS 10

static uint32_t s_enabled[(CONFIG_NUM_IRQS + 31) / 32];

#define DEEPWFI_DIV_MASK \
  (HPSYS_RCC_DWCFGR_HDIV_MASK | HPSYS_RCC_DWCFGR_PDIV1_MASK | HPSYS_RCC_DWCFGR_PDIV2_MASK)
// The boot dividers init.c programs: HCLK = 48 MHz / 12 = 4 MHz, PCLK1 = 4 MHz, PCLK2 = 2 MHz.
#define DEEPWFI_DIVS_DEFAULT                         \
  (PBL_FIELD_PREP(HPSYS_RCC_DWCFGR_HDIV_MASK, 12U) | \
   PBL_FIELD_PREP(HPSYS_RCC_DWCFGR_PDIV1_MASK, 0U) | \
   PBL_FIELD_PREP(HPSYS_RCC_DWCFGR_PDIV2_MASK, 1U))
// While audio DMA runs: HCLK stays at 48 MHz, PCLK1 = 48 MHz, PCLK2 = 24 MHz.
#define DEEPWFI_DIVS_AUDIO                           \
  (PBL_FIELD_PREP(HPSYS_RCC_DWCFGR_HDIV_MASK, 1U) |  \
   PBL_FIELD_PREP(HPSYS_RCC_DWCFGR_PDIV1_MASK, 0U) | \
   PBL_FIELD_PREP(HPSYS_RCC_DWCFGR_PDIV2_MASK, 1U))

//! Audio DMA users that need the deep-WFI clock profile described in idle.h.
static uint32_t s_audio_profile_holders;

//! Program the clock tree for the next deep WFI, with IRQs locked.
static void prv_apply_deep_wfi_profile(void) {
  if (s_audio_profile_holders > 0U) {
    // The vendor's audio power scenario: keep HCLK at the full deep-WFI source and force the HP
    // clock on, so the PDM DMA keeps moving while the core has its pipeline stopped.
    pbl_modifyl(DEEPWFI_DIV_MASK, DEEPWFI_DIVS_AUDIO, HPSYS_RCC(DWCFGR));
    pbl_modifyl(0U, HPSYS_RCC_DBGR_FORCE_HP, HPSYS_RCC(DBGR));
  } else {
    pbl_modifyl(HPSYS_RCC_DBGR_FORCE_HP, 0U, HPSYS_RCC(DBGR));
    pbl_modifyl(DEEPWFI_DIV_MASK, DEEPWFI_DIVS_DEFAULT, HPSYS_RCC(DWCFGR));
  }
}

void soc_sf32lb_deep_wfi_audio_profile_acquire(void) {
  pbl_irq_lock();
  ++s_audio_profile_holders;
  prv_apply_deep_wfi_profile();
  pbl_irq_unlock();
}

void soc_sf32lb_deep_wfi_audio_profile_release(void) {
  pbl_irq_lock();
  PBL_ASSERTN(s_audio_profile_holders > 0U);
  --s_audio_profile_holders;
  prv_apply_deep_wfi_profile();
  pbl_irq_unlock();
}

// Leave AON as the only wakeup source, the other handlers run from flash
static void prv_irqs_aon_only(void) {
  for (pbl_irq_t irq = 0; irq < CONFIG_NUM_IRQS; irq++) {
    if (pbl_irq_is_enabled(irq)) {
      s_enabled[irq / 32] |= PBL_BIT(irq % 32);
      pbl_irq_disable(irq);
    } else {
      s_enabled[irq / 32] &= ~PBL_BIT(irq % 32);
    }
  }

  pbl_irq_enable(PBL_IRQN(AON));
}

// AON stays enabled: its handler clears the wakeup status, then disables it
static void prv_irqs_restore(void) {
  for (pbl_irq_t irq = 0; irq < CONFIG_NUM_IRQS; irq++) {
    if ((s_enabled[irq / 32] & PBL_BIT(irq % 32)) != 0U) {
      pbl_irq_enable(irq);
    }
  }
}

// Inline, unlike pbl_modifyl(): the flash is down
static PBL_ALWAYS_INLINE void prv_update(uint32_t clear, uint32_t set, uintptr_t addr) {
  pbl_writel((pbl_readl(addr) & ~clear) | set, addr);
}

PBL_SECTION_RAM static void prv_enter_wfi(void) {
  pbl_cpu_idle();
}

PBL_SECTION_RAM static void prv_enter_deepwfi(void) {
#ifdef CONFIG_RAM
  pbl_ram_quiesce(EXTRAM);
#endif
  pbl_cpu_idle_deep();
}

// Runs with the flash powered down
PBL_SECTION_RAM static void prv_deepsleep(void *arg) {
  pbl_idle_hooks_late_enter(SOC_SF32LB_IDLE_DEEPSLEEP);

  prv_update(0U, HPSYS_AON_ANACR_PA_ISO, HPSYS_AON(ANACR));
  prv_update(0U, HPSYS_AON_ANACR_VHP_ISO, HPSYS_AON(ANACR));

  prv_update(HPSYS_AON_ISSR_HP_ACTIVE, 0U, HPSYS_AON(ISSR));
  pbl_writel(PBL_FIELD_PREP(HPSYS_AON_PMR_MODE_MASK, HPSYS_AON_PMR_MODE_DEEP_SLEEP),
             HPSYS_AON(PMR));

  pbl_cpu_idle();
  for (int i = 0; i < DEEPSLEEP_EXIT_NOPS; i++) {
    __asm volatile("nop");
  }

  prv_update(HPSYS_AON_ANACR_PA_ISO, 0U, HPSYS_AON(ANACR));
  prv_update(HPSYS_AON_ANACR_VHP_ISO, 0U, HPSYS_AON(ANACR));

  prv_update(0U, HPSYS_AON_ISSR_HP_ACTIVE, HPSYS_AON(ISSR));
  pbl_writel(0U, HPSYS_AON(PMR));

  while ((pbl_readl(HPSYS_AON(ACR)) & HPSYS_AON_ACR_HXT48_RDY) == 0U) {
  }

  pbl_idle_hooks_late_exit(SOC_SF32LB_IDLE_DEEPSLEEP);
}

PBL_SECTION_RAM static void prv_enter_deepsleep(void) {
  prv_irqs_aon_only();
#ifdef CONFIG_RAM
  pbl_ram_suspend(EXTRAM);
#endif
  pbl_flash_power_down(FLASH, prv_deepsleep, nullptr);
#ifdef CONFIG_RAM
  pbl_ram_resume(EXTRAM);
#endif
  prv_irqs_restore();
}

const struct pbl_idle_state pbl_soc_idle_states[] = {
  [SOC_SF32LB_IDLE_WFI] = {.name = "wfi", .min_ticks = 1, .exit_ticks = 0, .enter = prv_enter_wfi},
  [SOC_SF32LB_IDLE_DEEPWFI] =
      {.name = "deepwfi", .min_ticks = 1, .exit_ticks = 0, .enter = prv_enter_deepwfi},
  [SOC_SF32LB_IDLE_DEEPSLEEP] = {
    .name = "deepsleep",
    .min_ticks = PBL_US_TO_TICKS_CEIL(PBL_DT_IDLE_STATE_DEEPSLEEP_MIN_RESIDENCY_US),
    .exit_ticks = PBL_US_TO_TICKS_CEIL(PBL_DT_IDLE_STATE_DEEPSLEEP_EXIT_LATENCY_US),
    .enter = prv_enter_deepsleep
  },
};
const size_t pbl_soc_num_idle_states = PBL_ARRAY_SIZE(pbl_soc_idle_states);

PBL_IRQ_DIRECT(AON, 0, PBL_IRQ_ZERO_LATENCY) {
  uint32_t status;

  pbl_irq_disable(PBL_IRQN(AON));
  pbl_writel(0U, HPSYS_AON(PMR));

  status = pbl_readl(HPSYS_AON(WSR)) & ~HPSYS_AON_WSR_PIN_ALL;
  pbl_writel(status | HPSYS_AON_WCR_AON, HPSYS_AON(WCR));
}

#ifdef CONFIG_SHELL
static bool s_force_wfi;

static int prv_cmd_wfi(const struct pbl_shell *sh, size_t argc, char **argv) {
  bool force = argv[1][0] == '1';

  if (force && !s_force_wfi) {
    pbl_idle_state_block(SOC_SF32LB_IDLE_DEEPWFI);
  } else if (!force && s_force_wfi) {
    pbl_idle_state_release(SOC_SF32LB_IDLE_DEEPWFI);
  }
  s_force_wfi = force;

  pbl_shell_print(sh, "WFI forced %s (deep WFI and deep sleep %s)", force ? "ON" : "OFF",
                  force ? "disabled" : "allowed");
  return 0;
}

PBL_SHELL_SUBCMD_ADD(sub_sys, wfi, nullptr, "Force plain WFI when idle <0|1>", prv_cmd_wfi, 2, 0);
#endif
