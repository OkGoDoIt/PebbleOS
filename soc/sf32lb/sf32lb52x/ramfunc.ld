/*
 * SPDX-FileCopyrightText: 2026 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

        *(.*l1_ret_text_*)
        *(.*l1_ret_rodata_*)
        *(.text.SystemPowerOnModeGet)
        *(.text.HAL_Init)
        *(.text.HAL_MspInit)
        *(.text.HAL_Delay)
        *(.text.HAL_Delay_us)
        *(.text.HAL_Delay_us_)
        *(.text.HAL_Delay_us2_)
