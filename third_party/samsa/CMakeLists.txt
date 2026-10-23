# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

file(GLOB sources CONFIGURE_DEPENDS samsa/src/*.c)
pbl_include_directories(samsa/include)
pbl_library_named(samsa)
pbl_library_kind(stlib)
pbl_library_sources(${sources})
pbl_library_include_directories(samsa/src)
