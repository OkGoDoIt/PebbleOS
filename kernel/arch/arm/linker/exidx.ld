/*
 * SPDX-FileCopyrightText: 2026 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

    /* No unwinder is linked, so the unwind index is never read. */
    /DISCARD/ : {
        *(.ARM.exidx .ARM.exidx.*)
    }
