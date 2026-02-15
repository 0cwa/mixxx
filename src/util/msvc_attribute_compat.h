// Copyright (C) 2024 Mixxx DJ Software
// SPDX-License-Identifier: GPL-2.0-or-later

// MSVC compatibility header for GCC __attribute__ syntax

#pragma once

// Define __attribute__ for MSVC to handle GCC/Clang attributes
// GCC uses __attribute__((x)) with double parentheses
// MSVC doesn't support this syntax, so we define it to expand to nothing
// This means we lose some compiler optimizations (like noinline), but code will compile
#if defined(_MSC_VER)
    #define __attribute__(...)
#endif
