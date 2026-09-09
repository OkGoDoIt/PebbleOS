/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//! Sleep levels, ordered from shallowest to deepest.
typedef enum {
  SOC_SF32LB_ACTIVE = 0,  //!< No sleep at all
  SOC_SF32LB_WFI,         //!< Light WFI
  SOC_SF32LB_DEEPWFI,     //!< Deep WFI
  SOC_SF32LB_DEEPSLEEP,   //!< Deep sleep
} SocSf32lbSleepLevel;

//! Block the given sleep level and every deeper level. For example,
//! soc_sf32lb_sleep_block(SOC_SF32LB_DEEPWFI) forbids deep WFI and deep sleep,
//! leaving plain WFI as the deepest permitted level. SOC_SF32LB_ACTIVE cannot
//! be blocked. Refcounted; balance each call with soc_sf32lb_sleep_release().
//! Safe to call concurrently. With no blocks, the deepest permitted level is
//! SOC_SF32LB_DEEPSLEEP.
void soc_sf32lb_sleep_block(SocSf32lbSleepLevel level);

//! Release a block taken with soc_sf32lb_sleep_block(level).
void soc_sf32lb_sleep_release(SocSf32lbSleepLevel level);

//! Deepest sleep level currently permitted (one step shallower than the
//! shallowest outstanding block).
SocSf32lbSleepLevel soc_sf32lb_sleep_max_level(void);

//! Deep WFI normally divides HCLK down to 4 MHz and lets the HPSYS clock gate
//! itself while the core sleeps, which stalls audio DMA (PDM capture, codec
//! playback) -- so audio drivers have had to forbid deep WFI for the whole of
//! a capture, leaving the core in plain WFI with the 240 MHz DLL and the
//! full-speed bus clocks running. Acquiring the audio profile selects the
//! vendor's alternative instead: HCLK stays at the 48 MHz deep-WFI source and
//! the HP clock is forced on, so the core can still drop into deep WFI
//! between DMA interrupts. Refcounted; balance every acquire with a release.
//! Callers still block SOC_SF32LB_DEEPSLEEP themselves: that powers HPSYS
//! down entirely, peripherals included.
void soc_sf32lb_deep_wfi_audio_profile_acquire(void);
void soc_sf32lb_deep_wfi_audio_profile_release(void);
