// Copyright (C) 2020-2026 Parabola Research Limited
// SPDX-License-Identifier: MPL-2.0

#pragma once

#if defined(_MSC_VER)
#define BUNGEE_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define BUNGEE_NOINLINE __attribute__((noinline))
#else
#define BUNGEE_NOINLINE
#endif
