#pragma once

/**
 * @file engine/core/Format.h
 * @brief Which printf dialect `__attribute__((format(...)))` should check against.
 *
 * The bare `printf` archetype means the target's C library, which on Windows is the MS
 * runtime -- no `%zu`, no `%llu`, and the engine uses both. Under MinGW every one becomes
 * "unknown conversion type character 'z'", plus `-Wformat` reporting the remaining
 * arguments as extras.
 *
 * `__MINGW_PRINTF_FORMAT` resolves to `gnu_printf` when `__USE_MINGW_ANSI_STDIO` is set,
 * which the toolchain file sets. That define matters more than the warnings: without it
 * `vsnprintf` returns -1 when sizing a null buffer, `Logger::vformat` reads that as failure
 * and returns `{}`, and every log line in the engine becomes an empty string.
 */

// Included for its side effect: `__MINGW_PRINTF_FORMAT` lives in mingw-w64's `_mingw.h`,
// reached transitively from any C library header. Testing for it before something has
// defined it silently takes the `else` branch and reinstates the bug above -- silently,
// because falling back to the bare archetype is what a non-Windows build correctly does.
#include <cstdio>

#ifdef __MINGW_PRINTF_FORMAT
#define SUBSTRATE_PRINTF_FORMAT __MINGW_PRINTF_FORMAT
#else
#define SUBSTRATE_PRINTF_FORMAT printf
#endif
