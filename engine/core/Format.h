#pragma once

/**
 * @file engine/core/Format.h
 * @brief Which printf dialect `__attribute__((format(...)))` should check against.
 *
 * One macro, and it exists because the answer differs by platform in a way that fails
 * *silently* in one direction.
 *
 * GCC's `printf` archetype means "the C library's printf" -- and when the target is
 * Windows, that is the Microsoft C runtime, which has no `%zu` and no `%llu`. The engine
 * uses `%zu` in about twenty places and `%llu` in six. Compiled with the bare `printf`
 * archetype under MinGW, every one of them is "unknown conversion type character 'z'",
 * and `-Wformat` then reports the *remaining* arguments as extras, so a single `%zu`
 * produces three warnings that all describe something other than the problem.
 *
 * MinGW's own headers publish `__MINGW_PRINTF_FORMAT` for exactly this. It resolves to
 * `gnu_printf` when `__USE_MINGW_ANSI_STDIO` is set -- which the toolchain file sets, and
 * which separately makes `vsnprintf` behave the way `Logger::vformat` needs. That second
 * half matters more than the warnings: the MS runtime's `vsnprintf` returns -1 when asked
 * to size a null buffer, `vformat` reads a negative length as failure and returns `{}`,
 * and the result is that every log line in the engine becomes an empty string with
 * nothing to grep for.
 *
 * A header for one macro, rather than the `#ifdef` repeated in each of the four files
 * that needs it. Four copies of a portability conditional is the drift the Rule of Threes
 * is about, and this is the narrowest scope every caller reaches: `Logger.h` and
 * `Profiler.h` are in different modules from `ui/Inspector.cpp`, and `Profiler.h`
 * including `Logger.h` to borrow it would be a header dependency invented to avoid a file.
 */

// Included for its side effect, which is the only reason a header this small has one:
// `__MINGW_PRINTF_FORMAT` lives in mingw-w64's `_mingw.h`, reached transitively from any C
// library header. Testing for it before something has defined it silently takes the `else`
// branch and reinstates the exact bug this file exists to fix -- with no error, because
// falling back to the bare `printf` archetype is what a non-Windows build correctly does.
#include <cstdio>

#ifdef __MINGW_PRINTF_FORMAT
#define SUBSTRATE_PRINTF_FORMAT __MINGW_PRINTF_FORMAT
#else
#define SUBSTRATE_PRINTF_FORMAT printf
#endif
