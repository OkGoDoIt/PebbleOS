/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/**
 * @defgroup soc_sf32lb SF32LB
 * @ingroup soc
 * @brief SiFli SF32LB SoC interfaces.
 *
 * A driver whose peripheral does not work in an idle state blocks it while busy:
 *
 * @code{.c}
 * pbl_idle_state_block(SOC_SF32LB_IDLE_DEEPSLEEP); // the PWM does not run in deep sleep
 * prv_pwm_start();
 * prv_pwm_wait_done();
 * prv_pwm_stop();
 * pbl_idle_state_release(SOC_SF32LB_IDLE_DEEPSLEEP);
 * @endcode
 * @{
 */

/** @brief Idle states, see pbl_idle_state_block(). */
enum soc_sf32lb_idle_state {
  /** Light WFI. */
  SOC_SF32LB_IDLE_WFI,
  /** Deep WFI. */
  SOC_SF32LB_IDLE_DEEPWFI,
  /** Deep sleep. */
  SOC_SF32LB_IDLE_DEEPSLEEP,
};

/**
 * @brief Select the audio clock profile for deep WFI.
 *
 * Deep WFI normally divides HCLK down to 4 MHz and lets the HPSYS clock gate itself while the
 * core sleeps, which stalls audio DMA, so audio drivers have had to block deep WFI for the whole
 * of a capture. With the audio profile, HCLK stays at the 48 MHz deep-WFI source and the HP
 * clock is forced on, so the core can still drop into deep WFI between DMA interrupts.
 * Refcounted; balance each call with soc_sf32lb_deep_wfi_audio_profile_release(). Callers still
 * block @ref SOC_SF32LB_IDLE_DEEPSLEEP themselves: it powers HPSYS down, peripherals included.
 */
void soc_sf32lb_deep_wfi_audio_profile_acquire(void);

/** @brief Release a profile taken with soc_sf32lb_deep_wfi_audio_profile_acquire(). */
void soc_sf32lb_deep_wfi_audio_profile_release(void);

/** @} */
