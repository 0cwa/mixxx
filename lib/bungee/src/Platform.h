#pragma once

#if defined(_MSC_VER)
#define BUNGEE_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define BUNGEE_NOINLINE __attribute__((noinline))
#else
#define BUNGEE_NOINLINE
#endif
